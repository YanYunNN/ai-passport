#include "kiro_passport_network.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
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
#include "mbedtls/base64.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint8_t s_active_image_data[32768];
static size_t s_active_image_size = 0;
static char s_active_image_id[64] = {0};
static char s_active_image_title[64] = {0};
static uint32_t s_active_image_version = 0;
static SemaphoreHandle_t s_image_lock = NULL;
static char *s_rx_buffer = NULL;
static size_t s_rx_len = 0;

bool kiro_passport_network_get_image(kiro_passport_image_info_t *out_info)
{
    if (!out_info || s_active_image_size == 0) return false;
    if (!s_image_lock) s_image_lock = xSemaphoreCreateMutex();
    if (s_image_lock) xSemaphoreTake(s_image_lock, portMAX_DELAY);
    out_info->size = s_active_image_size;
    out_info->data = s_active_image_data;
    out_info->version = s_active_image_version;
    strlcpy(out_info->id, s_active_image_id, sizeof(out_info->id));
    strlcpy(out_info->title, s_active_image_title, sizeof(out_info->title));
    if (s_image_lock) xSemaphoreGive(s_image_lock);
    return true;
}

#define KIRO_NETWORK_NAMESPACE "passport"
#define KIRO_NETWORK_CONFIG_KEY "network"
#define KIRO_NETWORK_MESSAGE_MAX 512
#define KIRO_NETWORK_URI_MAX 192
#define KIRO_NETWORK_HEADER_MAX 224
#define KIRO_NETWORK_RECONNECT_MS 5000
#define KIRO_NETWORK_MIN_VALID_EPOCH 1704067200 /* 2024-01-01 UTC */
#define KIRO_ENROLLMENT_DEVICE_CODE_MAX 44
#define KIRO_ENROLLMENT_RESPONSE_MAX 512
#define KIRO_ENROLLMENT_URL "https://ws.yanyun.fun/v1/enrollment"
#define KIRO_ENROLLMENT_RELAY_URL "wss://ws.yanyun.fun"
#define KIRO_ENROLLMENT_VERIFICATION_URI "https://ws.yanyun.fun/admin/pair"
#define KIRO_ENROLLMENT_MAX_LIFETIME_SECONDS 600
#define KIRO_ENROLLMENT_MIN_INTERVAL_SECONDS 5
#define KIRO_ENROLLMENT_MAX_INTERVAL_SECONDS 60

static const char *TAG = "passport_wss";

typedef struct {
    char data[KIRO_ENROLLMENT_RESPONSE_MAX];
    size_t length;
    bool overflow;
} http_response_t;

typedef struct {
    kiro_passport_network_config_t config;
    esp_websocket_client_handle_t client;
    SemaphoreHandle_t lock;
    QueueHandle_t rejections;
    kiro_passport_network_state_t state;
    kiro_passport_enrollment_snapshot_t enrollment;
    char device_code[KIRO_ENROLLMENT_DEVICE_CODE_MAX];
    TickType_t enrollment_expires_at;
    TickType_t enrollment_next_poll_at;
    char session_id[KIRO_PASSPORT_SESSION_ID_MAX];
    char message[KIRO_NETWORK_MESSAGE_MAX];
    size_t message_length;
    volatile bool transport_failed;
} network_context_t;

static network_context_t s_network;

static void clear_secret(void *data, size_t length)
{
    volatile uint8_t *cursor = data;
    while (length--) *cursor++ = 0;
}

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

static bool valid_device_code(const char *value)
{
    if (!value || strlen(value) != KIRO_ENROLLMENT_DEVICE_CODE_MAX - 1) return false;
    for (const char *cursor = value; *cursor; cursor++) {
        if (!((*cursor >= 'A' && *cursor <= 'Z') || (*cursor >= 'a' && *cursor <= 'z') ||
              (*cursor >= '0' && *cursor <= '9') || *cursor == '-' || *cursor == '_')) return false;
    }
    return true;
}

static bool valid_user_code(const char *value)
{
    if (!value || strlen(value) != KIRO_PASSPORT_USER_CODE_MAX - 1) return false;
    for (const char *cursor = value; *cursor; cursor++) {
        if (*cursor < '0' || *cursor > '9') return false;
    }
    return true;
}

static bool valid_interval(const cJSON *value, uint32_t *interval)
{
    if (!cJSON_IsNumber(value) || value->valuedouble != (double)value->valueint ||
        value->valueint < KIRO_ENROLLMENT_MIN_INTERVAL_SECONDS ||
        value->valueint > KIRO_ENROLLMENT_MAX_INTERVAL_SECONDS) return false;
    *interval = (uint32_t)value->valueint;
    return true;
}

static bool ticks_reached(TickType_t deadline)
{
    return (int32_t)(xTaskGetTickCount() - deadline) >= 0;
}

static void set_state(kiro_passport_network_state_t state)
{
    xSemaphoreTake(s_network.lock, portMAX_DELAY);
    if (s_network.state != state) {
        ESP_LOGI(TAG, "Relay 网络状态变迁: %s (%d) -> %s (%d)",
                 kiro_passport_network_state_name(s_network.state), s_network.state,
                 kiro_passport_network_state_name(state), state);
        s_network.state = state;
    }
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
    if (!message) return;

    // 检查是否为图片推送消息 {"v":1,"type":"image",...}
    if (strstr(message, "\"type\":\"image\"") || strstr(message, "\"type\": \"image\"")) {
        const char *data_key = strstr(message, "\"data\":");
        if (data_key) {
            const char *data_start = strchr(data_key, '"');
            if (data_start) {
                data_start++; // 跳过首引号
                const char *data_end = strchr(data_start, '"');
                if (data_end && data_end > data_start) {
                    size_t b64_len = (size_t)(data_end - data_start);
                    size_t olen = 0;

                    if (!s_image_lock) s_image_lock = xSemaphoreCreateMutex();
                    if (s_image_lock) xSemaphoreTake(s_image_lock, portMAX_DELAY);

                    int ret = mbedtls_base64_decode(
                        s_active_image_data, sizeof(s_active_image_data), &olen,
                        (const unsigned char *)data_start, b64_len);

                    if (ret == 0 && olen > 0) {
                        s_active_image_size = olen;
                        s_active_image_version++;

                        // 提取 id
                        const char *id_key = strstr(message, "\"id\":");
                        if (id_key) {
                            const char *is = strchr(id_key, '"');
                            if (is) {
                                is++;
                                const char *ie = strchr(is, '"');
                                if (ie && ie > is) {
                                    size_t l = (size_t)(ie - is);
                                    if (l >= sizeof(s_active_image_id)) l = sizeof(s_active_image_id) - 1;
                                    memcpy(s_active_image_id, is, l);
                                    s_active_image_id[l] = '\0';
                                }
                            }
                        }

                        // 提取 title
                        const char *title_key = strstr(message, "\"title\":");
                        if (title_key) {
                            const char *ts = strchr(title_key, '"');
                            if (ts) {
                                ts++;
                                const char *te = strchr(ts, '"');
                                if (te && te > ts) {
                                    size_t l = (size_t)(te - ts);
                                    if (l >= sizeof(s_active_image_title)) l = sizeof(s_active_image_title) - 1;
                                    memcpy(s_active_image_title, ts, l);
                                    s_active_image_title[l] = '\0';
                                }
                            }
                        }

                        ESP_LOGI(TAG, "已成功接收并解码新图片: id=%s, title=%s, size=%zu bytes, v=%lu",
                                 s_active_image_id, s_active_image_title, s_active_image_size, (unsigned long)s_active_image_version);
                    } else {
                        ESP_LOGE(TAG, "图片 Base64 解码失败: ret=%d, b64_len=%zu", ret, b64_len);
                    }

                    if (s_image_lock) xSemaphoreGive(s_image_lock);
                    return;
                }
            }
        }
    }

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
        ESP_LOGI(TAG, "Relay WebSocket 已连接成功! Session ID: %s", s_network.session_id);
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
        if (length > 0 && length < (int)sizeof(hello)) {
            ESP_LOGI(TAG, "发送 WebSocket Hello 握手: %s", hello);
            send_text(hello);
        }
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "Relay WebSocket 连接断开 (DISCONNECTED)");
        kiro_passport_set_connection(false, NULL);
        s_network.transport_failed = true;
        if (wifi_manager_get_state() == WIFI_MANAGER_CONNECTED) set_state(KIRO_PASSPORT_NETWORK_CONNECTING);
    } else if (event_id == WEBSOCKET_EVENT_CLOSED) {
        ESP_LOGW(TAG, "Relay WebSocket 会话关闭 (CLOSED)");
        kiro_passport_set_connection(false, NULL);
        s_network.transport_failed = true;
        if (wifi_manager_get_state() == WIFI_MANAGER_CONNECTED) set_state(KIRO_PASSPORT_NETWORK_CONNECTING);
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        ESP_LOGE(TAG, "Relay WebSocket 发生错误 (ERROR)");
        kiro_passport_set_connection(false, NULL);
        s_network.transport_failed = true;
        set_state(KIRO_PASSPORT_NETWORK_ERROR);
    } else if (event_id == WEBSOCKET_EVENT_DATA) {
        ESP_LOGI(TAG, "Relay 收到数据 (len=%d, op=%d, offset=%d, total=%d, fin=%d)",
                 (int)event->data_len, event->op_code, (int)event->payload_offset, (int)event->payload_len, (int)event->fin);

        if (event->op_code == WS_TRANSPORT_OPCODES_PING || event->op_code == WS_TRANSPORT_OPCODES_PONG) {
            return;
        }

        if ((event->op_code != WS_TRANSPORT_OPCODES_TEXT && event->op_code != WS_TRANSPORT_OPCODES_CONT && event->op_code != 0) ||
            event->payload_len <= 0 || event->payload_len >= 65536 || event->payload_offset < 0 ||
            event->data_len < 0 || event->payload_offset + event->data_len > event->payload_len) {
            if (s_rx_buffer) { free(s_rx_buffer); s_rx_buffer = NULL; }
            s_rx_len = 0;
            return;
        }
        if (event->payload_offset == 0 || !s_rx_buffer) {
            if (s_rx_buffer) free(s_rx_buffer);
            s_rx_buffer = malloc(event->payload_len + 1);
            s_rx_len = 0;
        }
        if (s_rx_buffer && (size_t)event->payload_offset == s_rx_len) {
            memcpy(s_rx_buffer + event->payload_offset, event->data_ptr, event->data_len);
            s_rx_len += (size_t)event->data_len;
        }
        if (s_rx_buffer && s_rx_len == (size_t)event->payload_len) {
            s_rx_buffer[s_rx_len] = '\0';
            ESP_LOGI(TAG, "Relay 完整消息接收完毕 (total=%zu bytes)，正在处理...", s_rx_len);
            process_message(s_rx_buffer);
            free(s_rx_buffer);
            s_rx_buffer = NULL;
            s_rx_len = 0;
        }
    }
}

static void destroy_client(void)
{
    if (!s_network.client) return;
    ESP_LOGI(TAG, "销毁 WebSocket 客户端");
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
    int header_length = snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n",
                                 s_network.config.credential);
    if (uri_length <= 0 || uri_length >= (int)sizeof(uri) || header_length <= 0 ||
        header_length >= (int)sizeof(headers)) return ESP_ERR_INVALID_SIZE;

    generate_session_id(s_network.session_id, sizeof(s_network.session_id));
    ESP_LOGI(TAG, "正在启动 WebSocket 连接: URI=%s", uri);
    const esp_websocket_client_config_t config = {
        .uri = uri,
        .headers = headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = true,
        .enable_close_reconnect = false,
        .reconnect_timeout_ms = KIRO_NETWORK_RECONNECT_MS,
        .network_timeout_ms = 10000,
        .buffer_size = 4096,
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
    if (!s_network.client) {
        ESP_LOGE(TAG, "WebSocket 客户端内存初始化失败");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = esp_websocket_register_events(s_network.client, WEBSOCKET_EVENT_ANY,
                                                      websocket_event, NULL);
    if (result == ESP_OK) result = esp_websocket_client_start(s_network.client);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket 启动失败: %s", esp_err_to_name(result));
        destroy_client();
    }
    return result;
}

static esp_err_t http_response_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data || event->data_len <= 0) {
        return ESP_OK;
    }
    http_response_t *response = event->user_data;
    if (response->length + (size_t)event->data_len >= sizeof(response->data)) {
        response->overflow = true;
        return ESP_FAIL;
    }
    memcpy(response->data + response->length, event->data, event->data_len);
    response->length += (size_t)event->data_len;
    response->data[response->length] = '\0';
    return ESP_OK;
}

static esp_err_t post_enrollment(const char *path, const char *body, http_response_t *response,
                                 int *status)
{
    char url[sizeof(KIRO_ENROLLMENT_URL) + 16];
    int length = snprintf(url, sizeof(url), "%s/%s", KIRO_ENROLLMENT_URL, path);
    if (length <= 0 || length >= (int)sizeof(url)) return ESP_ERR_INVALID_SIZE;
    memset(response, 0, sizeof(*response));
    const esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_response_event,
        .user_data = response,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t result = esp_http_client_perform(client);
    if (result == ESP_OK) *status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return result == ESP_OK && !response->overflow ? ESP_OK :
        (result == ESP_OK ? ESP_ERR_INVALID_RESPONSE : result);
}

static cJSON *parse_json(const char *body)
{
    const char *end = NULL;
    return cJSON_ParseWithOpts(body, &end, 1);
}

static bool exact_object(const cJSON *root, int expected_members)
{
    return cJSON_IsObject(root) && cJSON_GetArraySize(root) == expected_members;
}

static bool parse_device_code_response(const char *body, char *device_code, char *user_code,
                                       uint32_t *expires_in, uint32_t *interval)
{
    cJSON *root = parse_json(body);
    cJSON *device = root ? cJSON_GetObjectItemCaseSensitive(root, "device_code") : NULL;
    cJSON *user = root ? cJSON_GetObjectItemCaseSensitive(root, "user_code") : NULL;
    cJSON *verification = root ? cJSON_GetObjectItemCaseSensitive(root, "verification_uri") : NULL;
    cJSON *expires = root ? cJSON_GetObjectItemCaseSensitive(root, "expires_in") : NULL;
    cJSON *poll_interval = root ? cJSON_GetObjectItemCaseSensitive(root, "interval") : NULL;
    cJSON *device_id = root ? cJSON_GetObjectItemCaseSensitive(root, "device_id") : NULL;
    bool valid = exact_object(root, 6) && cJSON_IsString(device) && valid_device_code(device->valuestring) &&
                 cJSON_IsString(user) && valid_user_code(user->valuestring) &&
                 cJSON_IsString(verification) &&
                 strcmp(verification->valuestring, KIRO_ENROLLMENT_VERIFICATION_URI) == 0 &&
                 cJSON_IsString(device_id) && strcmp(device_id->valuestring, s_network.config.device_id) == 0 &&
                 cJSON_IsNumber(expires) && expires->valuedouble == (double)expires->valueint &&
                 expires->valueint > 0 && expires->valueint <= KIRO_ENROLLMENT_MAX_LIFETIME_SECONDS &&
                 valid_interval(poll_interval, interval);
    if (valid) {
        snprintf(device_code, KIRO_ENROLLMENT_DEVICE_CODE_MAX, "%s", device->valuestring);
        snprintf(user_code, KIRO_PASSPORT_USER_CODE_MAX, "%s", user->valuestring);
        *expires_in = (uint32_t)expires->valueint;
    }
    cJSON_Delete(root);
    return valid;
}

static bool parse_pending_response(const char *body, const char *error_name, uint32_t *interval)
{
    cJSON *root = parse_json(body);
    cJSON *error = root ? cJSON_GetObjectItemCaseSensitive(root, "error") : NULL;
    cJSON *poll_interval = root ? cJSON_GetObjectItemCaseSensitive(root, "interval") : NULL;
    bool valid = exact_object(root, 2) && cJSON_IsString(error) &&
                 strcmp(error->valuestring, error_name) == 0 && valid_interval(poll_interval, interval);
    cJSON_Delete(root);
    return valid;
}

static bool parse_credential_response(const char *body, const char *device_id, char *credential)
{
    cJSON *root = parse_json(body);
    cJSON *response_device_id = root ? cJSON_GetObjectItemCaseSensitive(root, "device_id") : NULL;
    cJSON *response_credential = root ? cJSON_GetObjectItemCaseSensitive(root, "credential") : NULL;
    cJSON *version = root ? cJSON_GetObjectItemCaseSensitive(root, "credential_version") : NULL;
    bool valid = exact_object(root, 3) && cJSON_IsString(response_device_id) &&
                 strcmp(response_device_id->valuestring, device_id) == 0 &&
                 cJSON_IsString(response_credential) &&
                 safe_value(response_credential->valuestring, KIRO_PASSPORT_CREDENTIAL_MAX) &&
                 cJSON_IsNumber(version) && version->valuedouble == (double)version->valueint &&
                 version->valueint == 1;
    if (valid) snprintf(credential, KIRO_PASSPORT_CREDENTIAL_MAX, "%s", response_credential->valuestring);
    cJSON_Delete(root);
    return valid;
}

static void enrollment_fail(esp_err_t error)
{
    ESP_LOGE(TAG, "Enrollment 失败: %s (%d)", esp_err_to_name(error), error);
    xSemaphoreTake(s_network.lock, portMAX_DELAY);
    clear_secret(s_network.device_code, sizeof(s_network.device_code));
    s_network.enrollment.user_code[0] = '\0';
    s_network.enrollment.expires_in_seconds = 0;
    s_network.enrollment.last_error = error;
    s_network.enrollment.state = KIRO_PASSPORT_ENROLLMENT_ERROR;
    s_network.enrollment_expires_at = 0;
    s_network.enrollment_next_poll_at = 0;
    xSemaphoreGive(s_network.lock);
}

static void request_device_code(void)
{
    char device_id[KIRO_PASSPORT_DEVICE_ID_MAX];
    xSemaphoreTake(s_network.lock, portMAX_DELAY);
    if (s_network.enrollment.state != KIRO_PASSPORT_ENROLLMENT_REQUESTING) {
        xSemaphoreGive(s_network.lock);
        return;
    }
    snprintf(device_id, sizeof(device_id), "%s", s_network.config.device_id);
    xSemaphoreGive(s_network.lock);

    char request[64];
    int length = snprintf(request, sizeof(request), "{\"device_id\":\"%s\"}", device_id);
    http_response_t response;
    int status = 0;
    ESP_LOGI(TAG, "发起配对码申请: device_id=%s", device_id);
    esp_err_t result = length > 0 && length < (int)sizeof(request) ?
        post_enrollment("device-code", request, &response, &status) : ESP_ERR_INVALID_SIZE;
    clear_secret(request, sizeof(request));
    ESP_LOGI(TAG, "配对码申请响应: HTTP %d, err=%s, body=%s", status, esp_err_to_name(result), response.data);
    if (result != ESP_OK || status != 201) {
        clear_secret(&response, sizeof(response));
        enrollment_fail(result == ESP_OK ? ESP_ERR_INVALID_RESPONSE : result);
        return;
    }

    char device_code[KIRO_ENROLLMENT_DEVICE_CODE_MAX] = { 0 };
    char user_code[KIRO_PASSPORT_USER_CODE_MAX] = { 0 };
    uint32_t expires_in = 0;
    uint32_t interval = 0;
    bool valid = parse_device_code_response(response.data, device_code, user_code, &expires_in, &interval);
    clear_secret(&response, sizeof(response));
    xSemaphoreTake(s_network.lock, portMAX_DELAY);
    if (!valid || s_network.enrollment.state != KIRO_PASSPORT_ENROLLMENT_REQUESTING) {
        xSemaphoreGive(s_network.lock);
        clear_secret(device_code, sizeof(device_code));
        clear_secret(user_code, sizeof(user_code));
        if (!valid) enrollment_fail(ESP_ERR_INVALID_RESPONSE);
        return;
    }
    snprintf(s_network.device_code, sizeof(s_network.device_code), "%s", device_code);
    snprintf(s_network.enrollment.user_code, sizeof(s_network.enrollment.user_code), "%s", user_code);
    s_network.enrollment.expires_in_seconds = expires_in;
    s_network.enrollment.last_error = ESP_OK;
    s_network.enrollment.state = KIRO_PASSPORT_ENROLLMENT_WAITING_APPROVAL;
    s_network.enrollment_expires_at = xTaskGetTickCount() + pdMS_TO_TICKS(expires_in * 1000U);
    s_network.enrollment_next_poll_at = xTaskGetTickCount() + pdMS_TO_TICKS(interval * 1000U);
    ESP_LOGI(TAG, "已获得配对码: user_code=%s, 有效期=%lu秒, 轮询间隔=%lu秒", user_code, (unsigned long)expires_in, (unsigned long)interval);
    xSemaphoreGive(s_network.lock);
    clear_secret(device_code, sizeof(device_code));
    clear_secret(user_code, sizeof(user_code));
}

static void poll_device_code(void)
{
    char device_id[KIRO_PASSPORT_DEVICE_ID_MAX];
    char device_code[KIRO_ENROLLMENT_DEVICE_CODE_MAX];
    xSemaphoreTake(s_network.lock, portMAX_DELAY);
    if (s_network.enrollment.state != KIRO_PASSPORT_ENROLLMENT_WAITING_APPROVAL ||
        !ticks_reached(s_network.enrollment_next_poll_at)) {
        xSemaphoreGive(s_network.lock);
        return;
    }
    if (ticks_reached(s_network.enrollment_expires_at)) {
        xSemaphoreGive(s_network.lock);
        ESP_LOGW(TAG, "配对码已超时未完成审批");
        enrollment_fail(ESP_ERR_TIMEOUT);
        return;
    }
    snprintf(device_id, sizeof(device_id), "%s", s_network.config.device_id);
    snprintf(device_code, sizeof(device_code), "%s", s_network.device_code);
    xSemaphoreGive(s_network.lock);

    char request[128];
    int length = snprintf(request, sizeof(request),
                          "{\"device_id\":\"%s\",\"device_code\":\"%s\"}", device_id, device_code);
    http_response_t response;
    int status = 0;
    esp_err_t result = length > 0 && length < (int)sizeof(request) ?
        post_enrollment("token", request, &response, &status) : ESP_ERR_INVALID_SIZE;
    clear_secret(request, sizeof(request));
    clear_secret(device_code, sizeof(device_code));
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "轮询 Token 网络请求失败: %s", esp_err_to_name(result));
        xSemaphoreTake(s_network.lock, portMAX_DELAY);
        if (s_network.enrollment.state == KIRO_PASSPORT_ENROLLMENT_WAITING_APPROVAL) {
            s_network.enrollment_next_poll_at = xTaskGetTickCount() +
                pdMS_TO_TICKS(KIRO_ENROLLMENT_MIN_INTERVAL_SECONDS * 1000U);
        }
        xSemaphoreGive(s_network.lock);
        clear_secret(&response, sizeof(response));
        return;
    }

    uint32_t interval = 0;
    if (((status == 428 || status == 400) && parse_pending_response(response.data, "authorization_pending", &interval)) ||
        (status == 429 && parse_pending_response(response.data, "slow_down", &interval))) {
        ESP_LOGI(TAG, "轮询等待中 (status=%d, interval=%lu秒)", status, (unsigned long)interval);
        xSemaphoreTake(s_network.lock, portMAX_DELAY);
        if (s_network.enrollment.state == KIRO_PASSPORT_ENROLLMENT_WAITING_APPROVAL) {
            s_network.enrollment_next_poll_at = xTaskGetTickCount() + pdMS_TO_TICKS(interval * 1000U);
        }
        xSemaphoreGive(s_network.lock);
        clear_secret(&response, sizeof(response));
        return;
    }
    if (status != 201) {
        ESP_LOGE(TAG, "轮询收到非预期状态: status=%d, body=%s", status, response.data);
        clear_secret(&response, sizeof(response));
        enrollment_fail(ESP_ERR_INVALID_RESPONSE);
        return;
    }

    ESP_LOGI(TAG, "审批通过! 收到 201 凭证响应");
    char credential[KIRO_PASSPORT_CREDENTIAL_MAX] = { 0 };
    bool valid = parse_credential_response(response.data, device_id, credential);
    clear_secret(&response, sizeof(response));
    esp_err_t save_result = valid ? kiro_passport_network_configure(KIRO_ENROLLMENT_RELAY_URL, credential) :
                                    ESP_ERR_INVALID_RESPONSE;
    clear_secret(credential, sizeof(credential));
    ESP_LOGI(TAG, "配置保存结果: valid=%d, save_result=%s", (int)valid, esp_err_to_name(save_result));
    if (save_result != ESP_OK) {
        enrollment_fail(save_result);
        return;
    }
    xSemaphoreTake(s_network.lock, portMAX_DELAY);
    clear_secret(s_network.device_code, sizeof(s_network.device_code));
    s_network.enrollment = (kiro_passport_enrollment_snapshot_t){
        .state = KIRO_PASSPORT_ENROLLMENT_IDLE,
        .last_error = ESP_OK,
    };
    s_network.enrollment_expires_at = 0;
    s_network.enrollment_next_poll_at = 0;
    xSemaphoreGive(s_network.lock);
}

static void enrollment_step(void)
{
    xSemaphoreTake(s_network.lock, portMAX_DELAY);
    kiro_passport_enrollment_state_t state = s_network.enrollment.state;
    xSemaphoreGive(s_network.lock);
    if (state != KIRO_PASSPORT_ENROLLMENT_REQUESTING &&
        state != KIRO_PASSPORT_ENROLLMENT_WAITING_APPROVAL) return;
    if (wifi_manager_get_state() != WIFI_MANAGER_CONNECTED) return;
    if (time(NULL) < KIRO_NETWORK_MIN_VALID_EPOCH || time_sync_get_state() != TIME_SYNC_SUCCESS) {
        if (time_sync_get_state() != TIME_SYNC_SYNCING) time_sync_request();
        return;
    }
    if (state == KIRO_PASSPORT_ENROLLMENT_REQUESTING) request_device_code();
    else poll_device_code();
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

        enrollment_step();
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
}

esp_err_t kiro_passport_network_init(void)
{
    if (s_network.lock) return ESP_OK;
    s_network.lock = xSemaphoreCreateMutex();
    s_network.rejections = xQueueCreate(4, sizeof(kiro_passport_decision_t));
    if (!s_network.lock || !s_network.rejections) return ESP_ERR_NO_MEM;
    s_network.state = KIRO_PASSPORT_NETWORK_UNCONFIGURED;
    s_network.enrollment.state = KIRO_PASSPORT_ENROLLMENT_IDLE;
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
    clear_secret(&config, sizeof(config));
    return result;
}

esp_err_t kiro_passport_network_clear_configuration(void)
{
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(KIRO_NETWORK_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) result = nvs_erase_key(handle, KIRO_NETWORK_CONFIG_KEY);
    if (result == ESP_OK) result = nvs_commit(handle);
    if (handle) nvs_close(handle);
    if (result == ESP_OK) {
        xSemaphoreTake(s_network.lock, portMAX_DELAY);
        generate_device_id(s_network.config.device_id, sizeof(s_network.config.device_id));
        s_network.config.relay_url[0] = '\0';
        clear_secret(s_network.config.credential, sizeof(s_network.config.credential));
        xSemaphoreGive(s_network.lock);
    }
    return result;
}

bool kiro_passport_network_enrollment_supported(void)
{
    return true;
}

esp_err_t kiro_passport_network_start_enrollment(void)
{
    if (!s_network.lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_network.lock, portMAX_DELAY);
    if (s_network.config.credential[0] ||
        (s_network.enrollment.state != KIRO_PASSPORT_ENROLLMENT_IDLE &&
         s_network.enrollment.state != KIRO_PASSPORT_ENROLLMENT_ERROR)) {
        xSemaphoreGive(s_network.lock);
        return ESP_ERR_INVALID_STATE;
    }
    clear_secret(s_network.device_code, sizeof(s_network.device_code));
    s_network.enrollment = (kiro_passport_enrollment_snapshot_t){
        .state = KIRO_PASSPORT_ENROLLMENT_REQUESTING,
        .last_error = ESP_OK,
    };
    s_network.enrollment_expires_at = 0;
    s_network.enrollment_next_poll_at = 0;
    xSemaphoreGive(s_network.lock);
    return ESP_OK;
}

void kiro_passport_network_cancel_enrollment(void)
{
    if (!s_network.lock) return;
    xSemaphoreTake(s_network.lock, portMAX_DELAY);
    clear_secret(s_network.device_code, sizeof(s_network.device_code));
    s_network.enrollment = (kiro_passport_enrollment_snapshot_t){
        .state = KIRO_PASSPORT_ENROLLMENT_IDLE,
        .last_error = ESP_OK,
    };
    s_network.enrollment_expires_at = 0;
    s_network.enrollment_next_poll_at = 0;
    xSemaphoreGive(s_network.lock);
}

void kiro_passport_network_get_enrollment(kiro_passport_enrollment_snapshot_t *snapshot)
{
    if (!snapshot) return;
    if (!s_network.lock) {
        memset(snapshot, 0, sizeof(*snapshot));
        return;
    }
    xSemaphoreTake(s_network.lock, portMAX_DELAY);
    *snapshot = s_network.enrollment;
    if (snapshot->state == KIRO_PASSPORT_ENROLLMENT_WAITING_APPROVAL) {
        TickType_t now = xTaskGetTickCount();
        snapshot->expires_in_seconds = ticks_reached(s_network.enrollment_expires_at) ? 0 :
            (uint32_t)((s_network.enrollment_expires_at - now + configTICK_RATE_HZ - 1) /
                       configTICK_RATE_HZ);
    }
    xSemaphoreGive(s_network.lock);
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
