#include "wifi_manager.h"
#include "time_sync.h"
#include "esp_event.h"
#include "esp_eap_client.h"
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
#include <stdlib.h>
#include <string.h>
#define WIFI_PORTAL_MAX_FORM_LENGTH 320
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
static char s_failed_ssids[MAX_WIFI_PROFILES][33];
static uint8_t s_failed_count;
static bool s_rescan_running;

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

/* Track networks that failed in the current connect cycle so an automatic
 * fallback scan never picks the same SSID twice (avoids retry loops on wrong
 * credentials). Cleared on success or on a fresh manual/provisioning action. */
static void remember_failed_ssid(const char *ssid)
{
    if (!ssid || !ssid[0]) return;
    taskENTER_CRITICAL(&s_provisioning_lock);
    for (uint8_t i = 0; i < s_failed_count; i++) {
        if (strcmp(s_failed_ssids[i], ssid) == 0) {
            taskEXIT_CRITICAL(&s_provisioning_lock);
            return;
        }
    }
    if (s_failed_count < MAX_WIFI_PROFILES) {
        snprintf(s_failed_ssids[s_failed_count], sizeof(s_failed_ssids[0]),
                 "%s", ssid);
        s_failed_count++;
    }
    taskEXIT_CRITICAL(&s_provisioning_lock);
}

static void reset_failed_ssids(void)
{
    taskENTER_CRITICAL(&s_provisioning_lock);
    s_failed_count = 0;
    taskEXIT_CRITICAL(&s_provisioning_lock);
}

static bool failed_before(const char *ssid)
{
    if (!ssid) return false;
    taskENTER_CRITICAL(&s_provisioning_lock);
    bool found = false;
    for (uint8_t i = 0; i < s_failed_count; i++) {
        if (strcmp(s_failed_ssids[i], ssid) == 0) {
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_provisioning_lock);
    return found;
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

/* Keep the WPA2-Enterprise (EAP) state in sync with the station config that is
 * about to be used. The Wi-Fi driver only persists the plain station config to
 * flash, never the EAP credentials, so every connect path re-derives them from
 * the stored profile matching the configured SSID. Company networks that ask
 * for a login name use PEAP/MSCHAPv2; the stored identity is applied to both
 * the outer EAP identity and the inner MSCHAPv2 username. */
static esp_err_t sync_eap_mode(void)
{
    wifi_config_t current = { 0 };
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &current);
    if (err != ESP_OK) return err;

    if (current.sta.ssid[0]) {
        wifi_profile_t profile;
        if (wifi_nvs_load_profile((const char *)current.sta.ssid, &profile) == ESP_OK &&
            profile.enterprise) {
            err = esp_eap_client_set_identity((const unsigned char *)profile.identity,
                                              strlen(profile.identity));
            if (err == ESP_OK) {
                err = esp_eap_client_set_username((const unsigned char *)profile.identity,
                                                  strlen(profile.identity));
            }
            if (err == ESP_OK) {
                err = esp_eap_client_set_password((const unsigned char *)profile.password,
                                                  strlen(profile.password));
            }
            if (err == ESP_OK) err = esp_eap_client_set_eap_methods(ESP_EAP_TYPE_PEAP);
            if (err == ESP_OK) err = esp_wifi_sta_enterprise_enable();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "配置企业 Wi-Fi 认证失败: %s", esp_err_to_name(err));
            }
            memset(&profile, 0, sizeof(profile));
            return err;
        }
    }
    /* Personal networks (or no stored profile): keep enterprise mode off so a
     * later WPA2-PSK connect uses the normal 4-way handshake. */
    return esp_wifi_sta_enterprise_disable();
}

/* Shared "apply a station config and connect" sequence used by the portal
 * connect task and by the multi-profile switch path. */
static esp_err_t switch_station_and_connect(wifi_config_t *config)
{
    s_transitioning = true;
    esp_err_t result = stop_wifi();
    if (result == ESP_OK) result = esp_wifi_set_mode(WIFI_MODE_STA);

    /* Persist before starting the station: a reset, DHCP failure, or lost GOT_IP event
     * must not discard a valid selection. Invalid credentials can always be replaced by
     * re-provisioning from Settings. */
    if (result == ESP_OK) result = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (result == ESP_OK) result = esp_wifi_set_config(WIFI_IF_STA, config);
    if (result == ESP_OK) result = sync_eap_mode();
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "候选 Wi-Fi 凭据已在连接前写入 Flash");
        result = esp_wifi_start();
        if (result == ESP_OK) {
            esp_wifi_set_ps(s_power_save_enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
        }
    }
    s_transitioning = false;

    if (result != ESP_OK) return result;
    s_state = WIFI_MANAGER_CONNECTING;
    s_retry_count = 0;
    return esp_wifi_connect();
}

static void connect_task(void *argument)
{
    uint32_t generation = (uint32_t)(uintptr_t)argument;

    vTaskDelay(pdMS_TO_TICKS(250));
    if (!claim_connection_transition(generation)) {
        vTaskDelete(NULL);
        return;
    }

    stop_portal_server();
    esp_err_t result = switch_station_and_connect(&s_station_config);
    if (result != ESP_OK) fail_connection("启动候选 Wi-Fi 配置", result);
    vTaskDelete(NULL);
}

/* Background task for the automatic fallback scan started when retries for the
 * last used network are exhausted (e.g. the device moved to another place). */
static void wifi_rescan_task(void *argument)
{
    (void)argument;
    esp_err_t err = wifi_manager_select_best_profile();
    taskENTER_CRITICAL(&s_provisioning_lock);
    s_rescan_running = false;
    taskEXIT_CRITICAL(&s_provisioning_lock);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "自动切换网络失败: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

static esp_err_t submit_station_config(const char *ssid, const char *username,
                                       const char *password, bool enterprise,
                                       uint32_t *generation)
{
    if (!ssid[0] || strlen(ssid) > 32) return ESP_ERR_INVALID_ARG;
    if (enterprise) {
        /* Company networks: the login name is required, the EAP password has
         * no 8-character minimum (unlike a WPA2-PSK passphrase). */
        if (!username[0] || strlen(username) > 64 ||
            !password[0] || strlen(password) > 63) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (strlen(password) < 8 || strlen(password) > 63) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t station_config = { 0 };
    snprintf((char *)station_config.sta.ssid, sizeof(station_config.sta.ssid),
             "%s", ssid);
    if (!enterprise) {
        snprintf((char *)station_config.sta.password,
                 sizeof(station_config.sta.password), "%s", password);
    }
    station_config.sta.threshold.authmode = enterprise ? WIFI_AUTH_WPA2_ENTERPRISE
                                                       : WIFI_AUTH_WPA2_PSK;

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
    /* Single-page setup form served straight from flash: no external assets,
     * no heap allocations, so the nicer styling costs only ~3 KB of flash. */
    static const char page[] =
        "<!doctype html><html lang=en><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>FoloToy Wi-Fi Setup</title><style>"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{font-family:-apple-system,Segoe UI,Roboto,Arial,sans-serif;background:linear-gradient(160deg,#0f2027,#203a43,#2c5364);min-height:100vh;display:flex;align-items:center;justify-content:center;padding:16px;color:#e8f1f8}"
        ".card{background:#fff;border-radius:16px;box-shadow:0 12px 40px rgba(0,0,0,.35);padding:24px 20px;width:100%;max-width:340px}"
        ".brand{display:flex;align-items:center;gap:10px;margin-bottom:4px}"
        ".logo{width:38px;height:38px;border-radius:10px;background:#0e7cd4;display:flex;align-items:center;justify-content:center;flex:none}"
        ".brand h1{font-size:18px;color:#123}.brand p{font-size:12px;color:#678;margin-top:2px}"
        "label{display:block;font-size:13px;font-weight:600;color:#345;margin:14px 0 6px}"
        "input,select{width:100%;padding:10px 12px;border:1px solid #c8d4dd;border-radius:10px;font-size:15px;color:#123;background:#f7fafc;outline:none}"
        "input:focus,select:focus{border-color:#0e7cd4;background:#fff}"
        "button{width:100%;margin-top:20px;padding:12px;border:0;border-radius:10px;background:#0e7cd4;color:#fff;font-size:16px;font-weight:600;cursor:pointer}"
        "button:active{background:#0a63aa}"
        ".note{font-size:12px;color:#89a;margin-top:14px;text-align:center}"
        "</style></head><body>"
        "<div class=card>"
        "<div class=brand><div class=logo><svg width=22 height=22 viewBox='0 0 24 24' fill=none stroke=#fff stroke-width=2 stroke-linecap=round><path d='M5 12.55a11 11 0 0 1 14.08 0'/><path d='M8.53 16.11a6 6 0 0 1 6.95 0'/><line x1=12 y1=20 x2=12.01 y2=20/></svg></div>"
        "<div><h1>FoloToy</h1><p>Wi-Fi Setup</p></div></div>"
        "<form method=post action=/configure autocomplete=off>"
        "<label for=ssid>Network name (SSID)</label>"
        "<input id=ssid name=ssid maxlength=32 placeholder='e.g. MyWiFi' required>"
        "<label for=auth>Security</label>"
        "<select id=auth name=auth onchange=onAuth()>"
        "<option value=psk>WPA2 password</option>"
        "<option value=ent>WPA2-Enterprise (username + password)</option>"
        "</select>"
        "<div id=ent>"
        "<label for=username>Username</label>"
        "<input id=username name=username maxlength=64 placeholder='e.g. zhangsan@corp.com'>"
        "</div>"
        "<label for=password>Password</label>"
        "<input id=password name=password type=password minlength=8 maxlength=63 autocomplete=current-password placeholder='Wi-Fi password' required>"
        "<button type=submit>Connect</button>"
        "</form>"
        "<p class=note>Open 192.168.4.1 · FoloToy AI Passport</p>"
        "</div>"
        "<script>function onAuth(){var e=document.getElementById('auth').value==='ent';"
        "document.getElementById('ent').style.display=e?'block':'none';"
        "document.getElementById('password').minLength=e?1:8;"
        "document.getElementById('password').placeholder=e?'EAP password':'';}"
        "onAuth();</script></body></html>";
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
    char username[65];
    char password[64];
    char auth[4];
    uint32_t generation;
    bool enterprise = form_value(form, "auth", auth, sizeof(auth)) &&
                      strcmp(auth, "ent") == 0;
    if (!form_value(form, "ssid", ssid, sizeof(ssid)) ||
        !form_value(form, "password", password, sizeof(password)) ||
        (enterprise && !form_value(form, "username", username, sizeof(username))) ||
        submit_station_config(ssid, enterprise ? username : "", password, enterprise,
                              &generation) != ESP_OK) {
        memset(password, 0, sizeof(password));
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid Wi-Fi credentials");
        return ESP_FAIL;
    }

    /* Remember the submitted network so the fallback scan can switch back to
     * it later even if this first connection attempt fails. */
    wifi_profile_t profile = { 0 };
    /* Precision limits the copy to the buffer size so a full-length SSID
     * (32 chars) cannot trigger the truncation warning. */
    snprintf(profile.ssid, sizeof(profile.ssid), "%.*s",
             (int)sizeof(profile.ssid) - 1, ssid);
    snprintf(profile.password, sizeof(profile.password), "%s", password);
    if (enterprise) {
        profile.enterprise = 1;
        /* Same precision trick as the SSID above: a full-length username
         * (64 chars) must not trigger the truncation warning. */
        snprintf(profile.identity, sizeof(profile.identity), "%.*s",
                 (int)sizeof(profile.identity) - 1, username);
    }
    memset(password, 0, sizeof(password));
    if (enterprise) memset(username, 0, sizeof(username));
    if (wifi_manager_add_profile(&profile) != ESP_OK) {
        ESP_LOGW(TAG, "无法保存 Wi-Fi 配置到 NVS");
    }
    memset(&profile, 0, sizeof(profile));

    httpd_resp_set_type(request, "text/html");
    esp_err_t result = httpd_resp_sendstr(
        request, "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>FoloToy Wi-Fi</title></head><body style='margin:0;font-family:-apple-system,Segoe UI,Roboto,Arial,sans-serif;"
        "background:linear-gradient(160deg,#0f2027,#203a43,#2c5364);min-height:100vh;display:flex;align-items:center;"
        "justify-content:center;color:#e8f1f8;padding:16px'>"
        "<div style='background:#fff;color:#345;border-radius:16px;padding:28px 24px;max-width:320px;text-align:center'>"
        "<h2 style='margin:0 0 8px;font-size:18px'>Connecting…</h2>"
        "<p style='margin:0;font-size:14px;line-height:1.6'>Return to the device screen to check the status.</p>"
        "</div></body></html>");
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
            /* The boot-time reconnect has no pending config, so the EAP mode
             * must be restored from the stored profile before connecting. */
            esp_err_t result = sync_eap_mode();
            if (result == ESP_OK) result = esp_wifi_connect();
            if (result != ESP_OK) fail_connection("启动后连接 Wi-Fi", result);
        }
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        /* Print the 802.11 disconnect reason on every drop: it is the fastest
         * way to tell credential/EAP failures (reason 15 or 202) apart from
         * "AP not found" (201) when diagnosing a corporate network. */
        wifi_event_sta_disconnected_t *disconnected =
            (wifi_event_sta_disconnected_t *)event_data;
        if (disconnected) {
            ESP_LOGW(TAG, "STA 断开连接, reason=%d", disconnected->reason);
        }
        if (s_state == WIFI_MANAGER_CONNECTING && s_retry_count < WIFI_CONNECT_RETRY_LIMIT) {
            s_retry_count++;
            esp_err_t result = esp_wifi_connect();
            if (result != ESP_OK) fail_connection("重试 Wi-Fi 连接", result);
        } else if (s_state == WIFI_MANAGER_CONNECTING) {
            ESP_LOGW(TAG, "Wi-Fi 连接重试已耗尽");
            s_state = WIFI_MANAGER_FAILED;

            /* Remember which network just failed so an automatic scan cannot
             * pick it again (prevents retry loops on wrong credentials). */
            wifi_config_t current = { 0 };
            if (esp_wifi_get_config(WIFI_IF_STA, &current) == ESP_OK &&
                current.sta.ssid[0]) {
                remember_failed_ssid((const char *)current.sta.ssid);
            }
            clear_pending_config();

            /* Fall back to scanning for another stored profile. */
            if (wifi_nvs_count_profiles() > 0) {
                taskENTER_CRITICAL(&s_provisioning_lock);
                bool spawn = !s_rescan_running;
                if (spawn) s_rescan_running = true;
                taskEXIT_CRITICAL(&s_provisioning_lock);
                if (spawn &&
                    xTaskCreate(wifi_rescan_task, "wifi_rescan", 4096, NULL, 5, NULL) != pdPASS) {
                    taskENTER_CRITICAL(&s_provisioning_lock);
                    s_rescan_running = false;
                    taskEXIT_CRITICAL(&s_provisioning_lock);
                    ESP_LOGW(TAG, "无法创建 Wi-Fi 自动切换任务");
                }
            }
        } else if (s_state == WIFI_MANAGER_CONNECTED) {
            /* Dropped while online (e.g. the device moved to another location):
             * retry from scratch, then fall back to scanning. */
            ESP_LOGW(TAG, "连接中断，正在重连");
            s_state = WIFI_MANAGER_CONNECTING;
            s_retry_count = 0;
            esp_err_t result = esp_wifi_connect();
            if (result != ESP_OK) fail_connection("恢复 Wi-Fi 连接", result);
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
    reset_failed_ssids();
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

    reset_failed_ssids();
    s_state = s_saved_config ? WIFI_MANAGER_CONNECTING : WIFI_MANAGER_UNCONFIGURED;
    if (s_saved_config) {
        result = sync_eap_mode();
        if (result == ESP_OK) result = esp_wifi_connect();
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

/* Multi-WiFi profile management */

esp_err_t wifi_manager_add_profile(const wifi_profile_t *profile)
{
    if (!profile) return ESP_ERR_INVALID_ARG;
    return wifi_nvs_save_profile(profile);
}

esp_err_t wifi_manager_remove_profile(const char *ssid)
{
    if (!ssid) return ESP_ERR_INVALID_ARG;
    return wifi_nvs_remove_profile(ssid);
}

/* Apply the given profile to the station and start connecting. Shared by the
 * manual selection and by the automatic fallback scan; unlike the portal flow
 * it does not require an active SoftAP. */
static esp_err_t switch_to_profile(const wifi_profile_t *profile)
{
    wifi_config_t config = { 0 };
    snprintf((char *)config.sta.ssid, sizeof(config.sta.ssid), "%s", profile->ssid);
    if (!profile->enterprise) {
        snprintf((char *)config.sta.password, sizeof(config.sta.password), "%s",
                 profile->password);
    }
    config.sta.threshold.authmode = profile->enterprise ? WIFI_AUTH_WPA2_ENTERPRISE
                                                        : WIFI_AUTH_WPA2_PSK;

    taskENTER_CRITICAL(&s_provisioning_lock);
    if (!s_initialized || !s_enabled || s_portal_active || s_transitioning ||
        s_pending_config) {
        taskEXIT_CRITICAL(&s_provisioning_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_station_config = config;
    s_pending_config = true;
    taskEXIT_CRITICAL(&s_provisioning_lock);

    esp_err_t err = switch_station_and_connect(&config);
    memset(&config, 0, sizeof(config));
    if (err != ESP_OK) fail_connection("切换 Wi-Fi 网络", err);
    return err;
}

esp_err_t wifi_manager_set_active_profile(const char *ssid)
{
    if (!ssid) return ESP_ERR_INVALID_ARG;
    wifi_profile_t profile;
    esp_err_t err = wifi_nvs_load_profile(ssid, &profile);
    if (err != ESP_OK) return err;
    /* The user's explicit choice wins the scan tie-breaker until changed. */
    wifi_nvs_set_active_ssid(ssid);
    reset_failed_ssids();
    return switch_to_profile(&profile);
}

static bool scan_contains_ssid(const wifi_ap_record_t *records, uint16_t count,
                               const char *ssid)
{
    for (uint16_t i = 0; i < count; i++) {
        if (strcmp((const char *)records[i].ssid, ssid) == 0) return true;
    }
    return false;
}

esp_err_t wifi_manager_select_best_profile(void)
{
    wifi_profile_t profiles[MAX_WIFI_PROFILES];
    size_t count = wifi_nvs_get_all_profiles(profiles, MAX_WIFI_PROFILES);
    if (count == 0) return ESP_ERR_NOT_FOUND;

    taskENTER_CRITICAL(&s_provisioning_lock);
    bool busy = !s_initialized || !s_enabled || s_portal_active ||
                s_transitioning || s_pending_config;
    taskEXIT_CRITICAL(&s_provisioning_lock);
    if (busy) return ESP_ERR_INVALID_STATE;

    wifi_scan_config_t scan_cfg = { 0 };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) return err;
    uint16_t ap_num = 0;
    err = esp_wifi_scan_get_ap_num(&ap_num);
    if (err != ESP_OK) return err;
    if (ap_num == 0) return ESP_ERR_NOT_FOUND;

    wifi_ap_record_t *records = malloc((size_t)ap_num * sizeof(*records));
    if (!records) return ESP_ERR_NO_MEM;
    err = esp_wifi_scan_get_ap_records(&ap_num, records);
    if (err != ESP_OK) {
        free(records);
        return err;
    }

    /* Prefer the manually selected profile when visible, otherwise the most
     * preferred (lowest priority number) visible profile. Networks that
     * already failed in this cycle are skipped. */
    char active_ssid[33];
    bool has_active =
        wifi_nvs_get_active_ssid(active_ssid, sizeof(active_ssid)) == ESP_OK;
    const wifi_profile_t *best = NULL;
    for (size_t i = 0; i < count; i++) {
        if (failed_before(profiles[i].ssid)) continue;
        if (has_active && strcmp(profiles[i].ssid, active_ssid) == 0 &&
            scan_contains_ssid(records, ap_num, profiles[i].ssid)) {
            best = &profiles[i];
            break;
        }
    }
    if (!best) {
        for (size_t i = 0; i < count; i++) {
            if (failed_before(profiles[i].ssid) ||
                !scan_contains_ssid(records, ap_num, profiles[i].ssid)) {
                continue;
            }
            if (!best || profiles[i].priority < best->priority) {
                best = &profiles[i];
            }
        }
    }
    free(records);
    if (!best) return ESP_ERR_NOT_FOUND;
    return switch_to_profile(best);
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
    reset_failed_ssids();

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
            result = sync_eap_mode();
            if (result == ESP_OK) result = esp_wifi_connect();
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
