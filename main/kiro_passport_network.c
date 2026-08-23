#include "kiro_passport_network.h"

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "time_sync.h"
#include "wifi_manager.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define KIRO_NETWORK_NAMESPACE "passport"
#define KIRO_NETWORK_CONFIG_KEY "network"
#define KIRO_NETWORK_MESSAGE_MAX 512
#define KIRO_NETWORK_URI_MAX 192
#define KIRO_NETWORK_HEADER_MAX 224
#define KIRO_NETWORK_RECONNECT_MS 5000
#define KIRO_NETWORK_MIN_VALID_EPOCH 1704067200 /* 2024-01-01 UTC */

static const char *TAG = "passport_wss";

typedef struct {
    kiro_passport_network_config_t config;
    esp_websocket_client_handle_t client;
    SemaphoreHandle_t lock;
    QueueHandle_t rejections;
    kiro_passport_network_state_t state;
    char session_id[KIRO_PASSPORT_SESSION_ID_MAX];
    char message[KIRO_NETWORK_MESSAGE_MAX];
    size_t message_length;
    volatile bool transport_failed;
} network_context_t;

static network_context_t s_network;

static bool safe_value(const char *value, size_t max_length)
{
    if (!value || !value[0] || strlen(value) >= max_length) return false;
    for (const char *cursor = value; *cursor; cursor++) {
        unsigned char character = (unsigned char)*cursor;
        if (character < 0x21 || character > 0x7e || character == '"' ||
            character == '\\' || character == '\r' || character == '\n') return false;
    }
    return true;
}

static bool valid_relay_url(const char *relay_url)
{
    if (!safe_value(relay_url, KIRO_PASSPORT_RELAY_URL_MAX)) return false;
    return strncmp(relay_url, "wss://", 6) == 0 && strchr(relay_url + 6, '/') == NULL;
}

static void set_state(kiro_passport_network_state_t state)
{
    xSemaphoreTake(s_network.lock, portMAX_DELAY);
    s_network.state = state;
    xSemaphoreGive(s_network.lock);
}

static bool is_configured(void)
{
    xSemaphoreTake(s_network.lock, portMAX_DELAY);
    bool configured = s_network.config.relay_url[0] && s_network.config.credential[0];
    xSemaphoreGive(s_network.lock);
    return configured;
}

static void generate_device_id(char *device_id, size_t device_id_size)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(device_id, device_id_size, "passport-%02X%02X%02X%02X%02X%02X", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
}

static void generate_session_id(char *session_id, size_t session_id_size)
{
    uint8_t random[16];
    esp_fill_random(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random) && i * 2 + 2 < session_id_size; i++) {
        snprintf(session_id + i * 2, session_id_size - i * 2, "%02x", random[i]);
    }
}

static int send_text(const char *message)
{
    if (!s_network.client || !esp_websocket_client_is_connected(s_network.client)) return -1;
    return esp_websocket_client_send_text(s_network.client, message, strlen(message),
                                          pdMS_TO_TICKS(1000));
}

static void queue_rejection(const kiro_passport_decision_t *rejection)
{
    if (rejection && rejection->request_id[0] &&
        xQueueSend(s_network.rejections, rejection, 0) != pdTRUE) {
        ESP_LOGW(TAG, "拒绝队列已满；Worker 将按其超时策略拒绝该请求");
    }
}

static void process_message(const char *message)
{
    kiro_passport_decision_t rejection;
    kiro_passport_request_result_t result = kiro_passport_submit_request(message, time(NULL), &rejection);
    if (result != KIRO_PASSPORT_REQUEST_ACCEPTED) queue_rejection(&rejection);
}

static void websocket_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *event = event_data;
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        kiro_passport_set_connection(true, s_network.session_id);
        set_state(KIRO_PASSPORT_NETWORK_CONNECTED);
        const char quote = '"';
        char hello[KIRO_NETWORK_MESSAGE_MAX];
        int length = snprintf(hello, sizeof(hello),
                              "{%cv%c:1,%ctype%c:%chello%c,%cdevice_id%c:%c%s%c,"
                              "%csession_id%c:%c%s%c}",
                              quote, quote,
                              quote, quote, quote, quote,
                              quote, quote, quote, s_network.config.device_id, quote,
                              quote, quote, quote, s_network.session_id, quote);
        if (length > 0 && length < (int)sizeof(hello)) send_text(hello);
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_CLOSED) {
        kiro_passport_set_connection(false, NULL);
        s_network.transport_failed = true;
        if (wifi_manager_get_state() == WIFI_MANAGER_CONNECTED) set_state(KIRO_PASSPORT_NETWORK_CONNECTING);
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        kiro_passport_set_connection(false, NULL);
        s_network.transport_failed = true;
        set_state(KIRO_PASSPORT_NETWORK_ERROR);
    } else if (event_id == WEBSOCKET_EVENT_DATA) {
        if (event->op_code != WS_TRANSPORT_OPCODES_TEXT || event->payload_len <= 0 ||
            event->payload_len >= KIRO_NETWORK_MESSAGE_MAX || event->payload_offset < 0 ||
            event->data_len < 0 || event->payload_offset + event->data_len > event->payload_len) {
            s_network.message_length = 0;
            return;
        }
        if (event->payload_offset == 0) s_network.message_length = 0;
        if ((size_t)event->payload_offset != s_network.message_length) {
            s_network.message_length = 0;
            return;
        }
        memcpy(s_network.message + event->payload_offset, event->data_ptr, event->data_len);
        s_network.message_length += (size_t)event->data_len;
        if (event->fin && s_network.message_length == (size_t)event->payload_len) {
            s_network.message[s_network.message_length] = '\0';
            process_message(s_network.message);
            s_network.message_length = 0;
        }
    }
}

static void destroy_client(void)
{
    if (!s_network.client) return;
    esp_websocket_client_stop(s_network.client);
    esp_websocket_client_destroy(s_network.client);
    s_network.client = NULL;
    kiro_passport_set_connection(false, NULL);
}

static esp_err_t start_client(void)
{
    char uri[KIRO_NETWORK_URI_MAX];
    char headers[KIRO_NETWORK_HEADER_MAX];
    int uri_length = snprintf(uri, sizeof(uri), "%s/device/%s", s_network.config.relay_url,
                              s_network.config.device_id);
    int header_length = snprintf(headers, sizeof(headers), "Authorization: Bearer %s%c%c",
                                 s_network.config.credential, 13, 10);
    if (uri_length <= 0 || uri_length >= (int)sizeof(uri) || header_length <= 0 ||
        header_length >= (int)sizeof(headers)) return ESP_ERR_INVALID_SIZE;

    generate_session_id(s_network.session_id, sizeof(s_network.session_id));
    const esp_websocket_client_config_t config = {
        .uri = uri,
        .headers = headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = true,
        .enable_close_reconnect = false,
        .reconnect_timeout_ms = KIRO_NETWORK_RECONNECT_MS,
        .network_timeout_ms = 10000,
        .buffer_size = KIRO_NETWORK_MESSAGE_MAX,
        .task_stack = 6144,
        .task_prio = 5,
        .ping_interval_sec = 20,
        .pingpong_timeout_sec = 10,
        .keep_alive_enable = true,
        .keep_alive_idle = 20,
        .keep_alive_interval = 10,
        .keep_alive_count = 3,
    };
    s_network.client = esp_websocket_client_init(&config);
    if (!s_network.client) return ESP_ERR_NO_MEM;
    esp_err_t result = esp_websocket_register_events(s_network.client, WEBSOCKET_EVENT_ANY,
                                                      websocket_event, NULL);
    if (result == ESP_OK) result = esp_websocket_client_start(s_network.client);
    if (result != ESP_OK) destroy_client();
    return result;
}

static void send_decision(const kiro_passport_decision_t *decision, const char *reason)
{
    const char quote = '"';
    char message[KIRO_NETWORK_MESSAGE_MAX];
    int length = snprintf(message, sizeof(message),
                          "{%cv%c:1,%ctype%c:%cdecision%c,%cdevice_id%c:%c%s%c,"
                          "%csession_id%c:%c%s%c,%crequest_id%c:%c%s%c,"
                          "%cdecision%c:%c%s%c,%creason%c:%c%s%c}",
                          quote, quote,
                          quote, quote, quote, quote,
                          quote, quote, quote, decision->device_id, quote,
                          quote, quote, quote, decision->session_id, quote,
                          quote, quote, quote, decision->request_id, quote,
                          quote, quote, quote, decision->allow ? "allow" : "deny", quote,
                          quote, quote, quote, reason, quote);
    if (length > 0 && length < (int)sizeof(message) && send_text(message) == length) {
        if (strcmp(reason, "user") == 0) kiro_passport_ack_decision(decision);
    }
}

static void network_task(void *argument)
{
    (void)argument;
    for (;;) {
        kiro_passport_expire(time(NULL));
        if (s_network.transport_failed) {
            s_network.transport_failed = false;
            destroy_client();
        }
        if (!is_configured()) {
            destroy_client();
            set_state(KIRO_PASSPORT_NETWORK_UNCONFIGURED);
        } else if (wifi_manager_get_state() != WIFI_MANAGER_CONNECTED) {
            destroy_client();
            set_state(KIRO_PASSPORT_NETWORK_WAITING_WIFI);
        } else if (time(NULL) < KIRO_NETWORK_MIN_VALID_EPOCH ||
                   time_sync_get_state() != TIME_SYNC_SUCCESS) {
            destroy_client();
            set_state(KIRO_PASSPORT_NETWORK_WAITING_CLOCK);
            if (time_sync_get_state() != TIME_SYNC_SYNCING) time_sync_request();
        } else if (!s_network.client) {
            set_state(KIRO_PASSPORT_NETWORK_CONNECTING);
            if (start_client() != ESP_OK) {
                set_state(KIRO_PASSPORT_NETWORK_ERROR);
                vTaskDelay(pdMS_TO_TICKS(KIRO_NETWORK_RECONNECT_MS));
                continue;
            }
        }

        kiro_passport_decision_t decision;
        if (kiro_passport_get_decision(&decision)) send_decision(&decision, "user");
        if (xQueueReceive(s_network.rejections, &decision, 0) == pdTRUE) {
            send_decision(&decision, "policy");
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static esp_err_t load_config(void)
{
    generate_device_id(s_network.config.device_id, sizeof(s_network.config.device_id));
#if defined(CONFIG_NVS_ENCRYPTION) && CONFIG_NVS_ENCRYPTION
    nvs_handle_t handle;
    esp_err_t result = nvs_open(KIRO_NETWORK_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;
    kiro_passport_network_config_t stored = { 0 };
    size_t size = sizeof(stored);
    result = nvs_get_blob(handle, KIRO_NETWORK_CONFIG_KEY, &stored, &size);
    nvs_close(handle);
    if (result == ESP_OK && size == sizeof(stored) && valid_relay_url(stored.relay_url) &&
        safe_value(stored.credential, sizeof(stored.credential))) {
        snprintf(stored.device_id, sizeof(stored.device_id), "%s", s_network.config.device_id);
        s_network.config = stored;
        return ESP_OK;
    }
    return result == ESP_OK ? ESP_ERR_INVALID_RESPONSE : result;
#else
    return ESP_OK;
#endif
}

esp_err_t kiro_passport_network_init(void)
{
    if (s_network.lock) return ESP_OK;
    s_network.lock = xSemaphoreCreateMutex();
    s_network.rejections = xQueueCreate(4, sizeof(kiro_passport_decision_t));
    if (!s_network.lock || !s_network.rejections) return ESP_ERR_NO_MEM;
    s_network.state = KIRO_PASSPORT_NETWORK_UNCONFIGURED;
    esp_err_t result = load_config();
    if (result != ESP_OK) return result;
    result = kiro_passport_init(s_network.config.device_id);
    if (result != ESP_OK) return result;
    if (xTaskCreate(network_task, "passport_wss", 6144, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void kiro_passport_network_get_config(kiro_passport_network_config_t *config)
{
    if (!config) return;
    if (!s_network.lock) {
        memset(config, 0, sizeof(*config));
        return;
    }
    xSemaphoreTake(s_network.lock, portMAX_DELAY);
    *config = s_network.config;
    xSemaphoreGive(s_network.lock);
}

esp_err_t kiro_passport_network_configure(const char *relay_url, const char *credential)
{
    if (!s_network.lock || !valid_relay_url(relay_url) || !safe_value(credential, KIRO_PASSPORT_CREDENTIAL_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
#if !defined(CONFIG_NVS_ENCRYPTION) || !CONFIG_NVS_ENCRYPTION
    ESP_LOGE(TAG, "拒绝保存设备凭据：必须先启用 NVS encryption");
    return ESP_ERR_NOT_SUPPORTED;
#else
    kiro_passport_network_config_t config = { 0 };
    generate_device_id(config.device_id, sizeof(config.device_id));
    snprintf(config.relay_url, sizeof(config.relay_url), "%s", relay_url);
    snprintf(config.credential, sizeof(config.credential), "%s", credential);
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(KIRO_NETWORK_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) result = nvs_set_blob(handle, KIRO_NETWORK_CONFIG_KEY, &config, sizeof(config));
    if (result == ESP_OK) result = nvs_commit(handle);
    if (handle) nvs_close(handle);
    if (result == ESP_OK) {
        xSemaphoreTake(s_network.lock, portMAX_DELAY);
        s_network.config = config;
        xSemaphoreGive(s_network.lock);
    }
    return result;
#endif
}

esp_err_t kiro_passport_network_clear_configuration(void)
{
#if !defined(CONFIG_NVS_ENCRYPTION) || !CONFIG_NVS_ENCRYPTION
    return ESP_ERR_NOT_SUPPORTED;
#else
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(KIRO_NETWORK_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) result = nvs_erase_key(handle, KIRO_NETWORK_CONFIG_KEY);
    if (result == ESP_OK) result = nvs_commit(handle);
    if (handle) nvs_close(handle);
    if (result == ESP_OK) {
        xSemaphoreTake(s_network.lock, portMAX_DELAY);
        generate_device_id(s_network.config.device_id, sizeof(s_network.config.device_id));
        s_network.config.relay_url[0] = '\0';
        s_network.config.credential[0] = '\0';
        xSemaphoreGive(s_network.lock);
    }
    return result;
#endif
}

kiro_passport_network_state_t kiro_passport_network_get_state(void)
{
    if (!s_network.lock) return KIRO_PASSPORT_NETWORK_ERROR;
    xSemaphoreTake(s_network.lock, portMAX_DELAY);
    kiro_passport_network_state_t state = s_network.state;
    xSemaphoreGive(s_network.lock);
    return state;
}

const char *kiro_passport_network_state_name(kiro_passport_network_state_t state)
{
    switch (state) {
    case KIRO_PASSPORT_NETWORK_UNCONFIGURED: return "Not enrolled";
    case KIRO_PASSPORT_NETWORK_WAITING_WIFI: return "Waiting for Wi-Fi";
    case KIRO_PASSPORT_NETWORK_WAITING_CLOCK: return "Syncing clock";
    case KIRO_PASSPORT_NETWORK_CONNECTING: return "Connecting relay";
    case KIRO_PASSPORT_NETWORK_CONNECTED: return "Relay ready";
    default: return "Relay error";
    }
}
