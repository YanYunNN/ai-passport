#include "wifi_manager.h"
#include "time_sync.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WIFI_PORTAL_MAX_FORM_LENGTH 160
#define WIFI_CONNECT_RETRY_LIMIT 4

static const char *TAG = "wifi_manager";

static bool s_initialized;
static bool s_enabled = true;
static bool s_power_save_enabled = true;
static volatile wifi_manager_state_t s_state = WIFI_MANAGER_UNAVAILABLE;
static bool s_portal_active;
static bool s_transitioning;
static bool s_saved_config;
static bool s_pending_config;
static bool s_connection_scheduled;
static uint32_t s_provisioning_generation;
static uint8_t s_retry_count;
static httpd_handle_t s_server;
static wifi_config_t s_station_config;
static char s_ap_ssid[33];
static char s_ap_password[13];
static portMUX_TYPE s_provisioning_lock = portMUX_INITIALIZER_UNLOCKED;

static void stop_portal_server(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}

static esp_err_t stop_wifi(void)
{
    esp_err_t result = esp_wifi_stop();
    return result == ESP_ERR_WIFI_NOT_STARTED ? ESP_OK : result;
}

static void clear_pending_config(void)
{
    taskENTER_CRITICAL(&s_provisioning_lock);
    s_pending_config = false;
    s_connection_scheduled = false;
    memset(&s_station_config, 0, sizeof(s_station_config));
    taskEXIT_CRITICAL(&s_provisioning_lock);
}

static int hex_value(char character)
{
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    return -1;
}

static bool form_value(const char *form, const char *key, char *value, size_t size)
{
    size_t key_length = strlen(key);
    const char *cursor = form;

    while (*cursor) {
        if ((cursor == form || cursor[-1] == '&') &&
            strncmp(cursor, key, key_length) == 0 && cursor[key_length] == '=') {
            size_t output_length = 0;
            cursor += key_length + 1;
            while (*cursor && *cursor != '&') {
                char decoded = *cursor++;
                if (decoded == '+') {
                    decoded = ' ';
                } else if (decoded == '%') {
                    if (!cursor[0] || !cursor[1]) return false;
                    int high = hex_value(*cursor++);
                    int low = hex_value(*cursor++);
                    if (high < 0 || low < 0) return false;
                    decoded = (char)((high << 4) | low);
                }
                if (output_length + 1 >= size) return false;
                value[output_length++] = decoded;
            }
            value[output_length] = '\0';
            return output_length > 0;
        }
        const char *separator = strchr(cursor, '&');
        if (!separator) break;
        cursor = separator + 1;
    }
    return false;
}

static bool claim_connection_transition(uint32_t generation)
{
    bool claimed = false;

    taskENTER_CRITICAL(&s_provisioning_lock);
    if (s_enabled && s_portal_active && s_connection_scheduled &&
        generation == s_provisioning_generation) {
        s_portal_active = false;
        s_connection_scheduled = false;
        claimed = true;
    }
    taskEXIT_CRITICAL(&s_provisioning_lock);
    return claimed;
}

static void fail_connection(const char *operation, esp_err_t result)
{
    ESP_LOGW(TAG, "%s 失败: %s", operation, esp_err_to_name(result));
    s_state = WIFI_MANAGER_FAILED;
    clear_pending_config();
}

static void connect_task(void *argument)
{
    uint32_t generation = (uint32_t)(uintptr_t)argument;

    vTaskDelay(pdMS_TO_TICKS(250));
    if (!claim_connection_transition(generation)) {
        vTaskDelete(NULL);
        return;
    }

    s_transitioning = true;
    stop_portal_server();
    esp_err_t result = stop_wifi();
    if (result == ESP_OK) result = esp_wifi_set_mode(WIFI_MODE_STA);

    /* Persist before starting the station: a reset, DHCP failure, or lost GOT_IP event
     * must not discard a valid provisioning submission. Invalid credentials can always
     * be replaced by starting provisioning again from Settings. */
    if (result == ESP_OK) result = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (result == ESP_OK) result = esp_wifi_set_config(WIFI_IF_STA, &s_station_config);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "候选 Wi-Fi 凭据已在连接前写入 Flash");
        result = esp_wifi_start();
        if (result == ESP_OK) {
            esp_wifi_set_ps(s_power_save_enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
        }
    }
    s_transitioning = false;

    if (result != ESP_OK) {
        fail_connection("启动候选 Wi-Fi 配置", result);
        vTaskDelete(NULL);
        return;
    }

    s_state = WIFI_MANAGER_CONNECTING;
    s_retry_count = 0;
    result = esp_wifi_connect();
    if (result != ESP_OK) fail_connection("连接候选 Wi-Fi", result);
    vTaskDelete(NULL);
}

static esp_err_t submit_station_config(const char *ssid, const char *password,
                                       uint32_t *generation)
{
    if (strlen(ssid) > 32 || strlen(password) < 8 || strlen(password) > 63) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t station_config = { 0 };
    snprintf((char *)station_config.sta.ssid, sizeof(station_config.sta.ssid),
             "%s", ssid);
    snprintf((char *)station_config.sta.password,
             sizeof(station_config.sta.password), "%s", password);
    station_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    taskENTER_CRITICAL(&s_provisioning_lock);
    if (!s_enabled || !s_portal_active || s_pending_config || s_connection_scheduled) {
        taskEXIT_CRITICAL(&s_provisioning_lock);
        memset(&station_config, 0, sizeof(station_config));
        return ESP_ERR_INVALID_STATE;
    }
    s_station_config = station_config;
    s_pending_config = true;
    *generation = s_provisioning_generation;
    taskEXIT_CRITICAL(&s_provisioning_lock);
    memset(&station_config, 0, sizeof(station_config));
    return ESP_OK;
}

static esp_err_t schedule_station_connection(uint32_t generation)
{
    taskENTER_CRITICAL(&s_provisioning_lock);
    if (!s_enabled || !s_portal_active || !s_pending_config ||
        generation != s_provisioning_generation || s_connection_scheduled) {
        taskEXIT_CRITICAL(&s_provisioning_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_connection_scheduled = true;
    taskEXIT_CRITICAL(&s_provisioning_lock);

    if (xTaskCreate(connect_task, "wifi_connect", 4096,
                    (void *)(uintptr_t)generation, 5, NULL) == pdPASS) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "无法创建 Wi-Fi 连接任务");
    clear_pending_config();
    return ESP_ERR_NO_MEM;
}

static esp_err_t portal_get_handler(httpd_req_t *request)
{
    static const char page[] =
        "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>FoloToy Wi-Fi</title></head><body><h2>FoloToy Wi-Fi Setup</h2>"
        "<form method=post action=/configure><label>Wi-Fi name (SSID)<br><input name=ssid maxlength=32 required></label><br>"
        "<label>Wi-Fi password<br><input type=password name=password minlength=8 maxlength=63 required></label><br>"
        "<button type=submit>Connect</button></form></body></html>";
    httpd_resp_set_type(request, "text/html");
    return httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t portal_configure_handler(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len > WIFI_PORTAL_MAX_FORM_LENGTH) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_FAIL;
    }

    char form[WIFI_PORTAL_MAX_FORM_LENGTH + 1];
    size_t received = 0;
    while (received < request->content_len) {
        int count = httpd_req_recv(request, form + received, request->content_len - received);
        if (count <= 0) {
            httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Incomplete request");
            return ESP_FAIL;
        }
        received += (size_t)count;
    }
    form[received] = '\0';

    char ssid[33];
    char password[64];
    uint32_t generation;
    if (!form_value(form, "ssid", ssid, sizeof(ssid)) ||
        !form_value(form, "password", password, sizeof(password)) ||
        submit_station_config(ssid, password, &generation) != ESP_OK) {
        memset(password, 0, sizeof(password));
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid Wi-Fi credentials");
        return ESP_FAIL;
    }
    memset(password, 0, sizeof(password));

    httpd_resp_set_type(request, "text/html");
    esp_err_t result = httpd_resp_sendstr(
        request, "<h2>Connecting</h2><p>Return to the device to check status.</p>");
    if (result != ESP_OK) return result;
    return schedule_station_connection(generation);
}

static esp_err_t start_portal_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 2;
    config.stack_size = 4096;
    config.lru_purge_enable = true;

    esp_err_t result = httpd_start(&s_server, &config);
    if (result != ESP_OK) return result;

    const httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = portal_get_handler, .user_ctx = NULL,
    };
    const httpd_uri_t configure = {
        .uri = "/configure", .method = HTTP_POST,
        .handler = portal_configure_handler, .user_ctx = NULL,
    };
    result = httpd_register_uri_handler(s_server, &root);
    if (result == ESP_OK) result = httpd_register_uri_handler(s_server, &configure);
    if (result != ESP_OK) stop_portal_server();
    return result;
}

static bool station_config_matches(const wifi_config_t *expected,
                                   const wifi_config_t *actual)
{
    return expected && actual &&
           memcmp(expected->sta.ssid, actual->sta.ssid, sizeof(expected->sta.ssid)) == 0;
}

static bool persist_pending_config(void)
{
    wifi_config_t saved_config = { 0 };
    esp_err_t result = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (result == ESP_OK) result = esp_wifi_set_config(WIFI_IF_STA, &s_station_config);
    if (result == ESP_OK) result = esp_wifi_get_config(WIFI_IF_STA, &saved_config);

    bool saved = result == ESP_OK && station_config_matches(&s_station_config, &saved_config);
    memset(&saved_config, 0, sizeof(saved_config));
    if (!saved) {
        if (result == ESP_OK) result = ESP_FAIL;
        ESP_LOGW(TAG, "Wi-Fi 凭据未写入 Flash: %s", esp_err_to_name(result));
        return false;
    }

    s_saved_config = true;
    ESP_LOGI(TAG, "Wi-Fi 凭据已写入 Flash，并已读取回验");
    clear_pending_config();
    return true;
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;
    (void)event_base;
    (void)event_data;
    if (!s_enabled || s_transitioning || s_portal_active) return;

    if (event_id == WIFI_EVENT_STA_START) {
        if (s_saved_config || s_pending_config) {
            s_state = WIFI_MANAGER_CONNECTING;
            esp_err_t result = esp_wifi_connect();
            if (result != ESP_OK) fail_connection("启动后连接 Wi-Fi", result);
        }
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_state == WIFI_MANAGER_CONNECTING && s_retry_count < WIFI_CONNECT_RETRY_LIMIT) {
            s_retry_count++;
            esp_err_t result = esp_wifi_connect();
            if (result != ESP_OK) fail_connection("重试 Wi-Fi 连接", result);
        } else if (s_state == WIFI_MANAGER_CONNECTING) {
            ESP_LOGW(TAG, "Wi-Fi 连接重试已耗尽");
            s_state = WIFI_MANAGER_FAILED;
            clear_pending_config();
        }
    }
}

static void ip_event_handler(void *argument, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)argument;
    (void)event_base;
    (void)event_id;
    (void)event_data;
    if (!s_enabled) return;

    if (s_pending_config) persist_pending_config();
    s_state = WIFI_MANAGER_CONNECTED;
    ESP_LOGI(TAG, "已获取 IP，网络已连接");

    /* The NTP worker is independent from LVGL; ui_status consumes its result safely. */
    esp_err_t result = time_sync_request();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "自动 NTP 同步无法启动: %s", esp_err_to_name(result));
    }
}

esp_err_t wifi_manager_set_enabled(bool enabled)
{
    s_enabled = enabled;
    if (!s_initialized) return ESP_OK;

    if (!enabled) {
        taskENTER_CRITICAL(&s_provisioning_lock);
        s_provisioning_generation++;
        s_portal_active = false;
        s_connection_scheduled = false;
        s_pending_config = false;
        memset(&s_station_config, 0, sizeof(s_station_config));
        taskEXIT_CRITICAL(&s_provisioning_lock);

        s_transitioning = true;
        stop_portal_server();
        esp_err_t result = stop_wifi();
        s_transitioning = false;
        if (result != ESP_OK) {
            s_state = WIFI_MANAGER_FAILED;
            ESP_LOGW(TAG, "关闭 Wi-Fi 失败: %s", esp_err_to_name(result));
            return result;
        }
        s_state = WIFI_MANAGER_DISABLED;
        ESP_LOGI(TAG, "Wi-Fi 已关闭；已保存的路由器凭据仍保留");
        return ESP_OK;
    }

    s_transitioning = true;
    esp_err_t result = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (result == ESP_OK) result = esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_config_t saved_config = { 0 };
    if (result == ESP_OK) result = esp_wifi_get_config(WIFI_IF_STA, &saved_config);
    if (result == ESP_OK) s_saved_config = saved_config.sta.ssid[0] != '\0';
    memset(&saved_config, 0, sizeof(saved_config));
    if (result == ESP_OK) {
        result = esp_wifi_start();
        if (result == ESP_OK) {
            esp_wifi_set_ps(s_power_save_enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
        }
    }
    s_transitioning = false;
    if (result != ESP_OK) {
        s_state = WIFI_MANAGER_FAILED;
        ESP_LOGW(TAG, "启用 Wi-Fi 失败: %s", esp_err_to_name(result));
        return result;
    }

    s_state = s_saved_config ? WIFI_MANAGER_CONNECTING : WIFI_MANAGER_UNCONFIGURED;
    if (s_saved_config) {
        result = esp_wifi_connect();
        if (result != ESP_OK) {
            fail_connection("重新连接已保存的 Wi-Fi", result);
            return result;
        }
        ESP_LOGI(TAG, "Wi-Fi 已启用，正在重连已保存网络");
    } else {
        ESP_LOGI(TAG, "Wi-Fi 已启用，尚未配置网络");
    }
    return ESP_OK;
}

bool wifi_manager_is_enabled(void)
{
    return s_enabled;
}

esp_err_t wifi_manager_init(void)
{
    if (s_initialized) return ESP_OK;

    esp_err_t result = nvs_flash_init();
    if (result != ESP_OK) return result;
    result = esp_netif_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
    if (!esp_netif_create_default_wifi_sta()) return ESP_ERR_NO_MEM;
    if (!esp_netif_create_default_wifi_ap()) return ESP_ERR_NO_MEM;

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&init_config);
    if (result != ESP_OK) return result;
    result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, NULL);
    if (result != ESP_OK) return result;
    result = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        ip_event_handler, NULL);
    if (result != ESP_OK) return result;

    result = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (result != ESP_OK) return result;
    result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result != ESP_OK) return result;

    wifi_config_t saved_config = { 0 };
    result = esp_wifi_get_config(WIFI_IF_STA, &saved_config);
    if (result != ESP_OK) return result;
    s_saved_config = saved_config.sta.ssid[0] != '\0';
    memset(&saved_config, 0, sizeof(saved_config));
    s_initialized = true;

    if (!s_enabled) {
        s_state = WIFI_MANAGER_DISABLED;
        ESP_LOGI(TAG, "Wi-Fi 按保存的开关状态保持关闭");
        return ESP_OK;
    }

    s_state = s_saved_config ? WIFI_MANAGER_CONNECTING : WIFI_MANAGER_UNCONFIGURED;
    result = esp_wifi_start();
    if (result != ESP_OK) {
        s_state = WIFI_MANAGER_FAILED;
        return result;
    }
    if (s_saved_config) {
        ESP_LOGI(TAG, "检测到已保存 Wi-Fi 凭据，正在自动重连");
    } else {
        ESP_LOGI(TAG, "未检测到已保存 Wi-Fi 凭据");
    }
    return ESP_OK;
}

wifi_manager_state_t wifi_manager_get_state(void)
{
    return s_state;
}

esp_err_t wifi_manager_get_connected_ssid(char *ssid, size_t size)
{
    if (!ssid || size == 0) return ESP_ERR_INVALID_ARG;
    ssid[0] = '\0';
    if (s_state != WIFI_MANAGER_CONNECTED) return ESP_ERR_INVALID_STATE;

    wifi_ap_record_t access_point = { 0 };
    esp_err_t result = esp_wifi_sta_get_ap_info(&access_point);
    if (result != ESP_OK) return result;

    size_t output_length = 0;
    for (size_t i = 0; i < sizeof(access_point.ssid) && access_point.ssid[i]; i++) {
        if (output_length + 1 >= size) break;
        uint8_t character = access_point.ssid[i];
        ssid[output_length++] = character >= 0x20u && character <= 0x7eu
                                  ? (char)character : '?';
    }
    ssid[output_length] = '\0';
    return ESP_OK;
}

esp_err_t wifi_manager_start_provisioning(void)
{
    if (!s_initialized || !s_enabled) return ESP_ERR_INVALID_STATE;

    uint32_t random = esp_random();
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "FoloToy-%04X", (unsigned)(random & 0xFFFFu));
    snprintf(s_ap_password, sizeof(s_ap_password), "Folo-%06u",
             (unsigned)(random % 1000000u));

    wifi_config_t ap_config = { 0 };
    size_t ap_ssid_length = strlen(s_ap_ssid);
    memcpy(ap_config.ap.ssid, s_ap_ssid, ap_ssid_length);
    snprintf((char *)ap_config.ap.password, sizeof(ap_config.ap.password), "%s",
             s_ap_password);
    ap_config.ap.ssid_len = ap_ssid_length;
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 1;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    taskENTER_CRITICAL(&s_provisioning_lock);
    s_provisioning_generation++;
    s_connection_scheduled = false;
    s_pending_config = false;
    memset(&s_station_config, 0, sizeof(s_station_config));
    taskEXIT_CRITICAL(&s_provisioning_lock);

    s_transitioning = true;
    stop_portal_server();
    esp_err_t result = stop_wifi();
    if (result == ESP_OK) result = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (result == ESP_OK) result = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (result == ESP_OK) result = esp_wifi_start();
    if (result == ESP_OK) result = start_portal_server();
    s_transitioning = false;

    if (result == ESP_OK) {
        taskENTER_CRITICAL(&s_provisioning_lock);
        s_portal_active = true;
        s_state = WIFI_MANAGER_PROVISIONING;
        taskEXIT_CRITICAL(&s_provisioning_lock);
        ESP_LOGI(TAG, "Wi-Fi 配网页已启动");
    } else {
        stop_portal_server();
        stop_wifi();
        s_state = WIFI_MANAGER_FAILED;
        ESP_LOGW(TAG, "启动 Wi-Fi 配网页失败: %s", esp_err_to_name(result));
    }
    return result;
}

void wifi_manager_stop_provisioning(void)
{
    taskENTER_CRITICAL(&s_provisioning_lock);
    bool portal_active = s_portal_active;
    if (portal_active) {
        s_provisioning_generation++;
        s_portal_active = false;
        s_connection_scheduled = false;
        s_pending_config = false;
        memset(&s_station_config, 0, sizeof(s_station_config));
    }
    taskEXIT_CRITICAL(&s_provisioning_lock);
    if (!portal_active) return;

    s_transitioning = true;
    stop_portal_server();
    esp_err_t result = stop_wifi();
    if (result == ESP_OK) result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result == ESP_OK && s_enabled) {
        result = esp_wifi_start();
        if (result == ESP_OK) {
            esp_wifi_set_ps(s_power_save_enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
        }
    }
    s_transitioning = false;
    if (result != ESP_OK) {
        s_state = WIFI_MANAGER_FAILED;
        ESP_LOGW(TAG, "停止 Wi-Fi 配网失败: %s", esp_err_to_name(result));
        return;
    }

    if (!s_enabled) {
        s_state = WIFI_MANAGER_DISABLED;
    } else {
        s_state = s_saved_config ? WIFI_MANAGER_CONNECTING : WIFI_MANAGER_UNCONFIGURED;
        if (s_saved_config) {
            result = esp_wifi_connect();
            if (result != ESP_OK) fail_connection("恢复已保存 Wi-Fi", result);
        }
    }
}

const char *wifi_manager_get_provisioning_ssid(void)
{
    return s_portal_active ? s_ap_ssid : "";
}

const char *wifi_manager_get_provisioning_password(void)
{
    return s_portal_active ? s_ap_password : "";
}

/* The Wi-Fi driver retains this policy across normal STA reconnects. It is applied
 * after init so an off-at-boot radio can keep the user's choice until enabled. */
esp_err_t wifi_manager_set_power_save(bool enabled)
{
    bool previous = s_power_save_enabled;
    s_power_save_enabled = enabled;

    if (!s_initialized || !s_enabled || s_portal_active) return ESP_OK;

    esp_err_t result = esp_wifi_set_ps(enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    if (result != ESP_OK) {
        s_power_save_enabled = previous;
        ESP_LOGW(TAG, "设置 Wi-Fi modem sleep 失败: %s", esp_err_to_name(result));
        return result;
    }

    ESP_LOGI(TAG, "Wi-Fi modem sleep %s", enabled ? "已启用" : "已关闭");
    return ESP_OK;
}

bool wifi_manager_is_power_save_enabled(void)
{
    return s_power_save_enabled;
}
