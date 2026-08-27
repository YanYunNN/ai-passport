/* simulator/src/stubs_kiro.c
 * Kiro Passport 的宿主桩（P0 无网络）。
 *
 * kiro_passport_network_init() 返回 ESP_ERR_NOT_SUPPORTED：
 * main.c 据此把菜单里的 Kiro 页标记为"不可用"，与固件断网降级一致。
 */
#include "kiro_passport.h"
#include "kiro_passport_network.h"

#include <string.h>

/* ==================== kiro_passport（审批状态机） ==================== */
static kiro_passport_snapshot_t s_snapshot = {
    .initialized = false,
    .connected = false,
    .pending = false,
    .state = "OFFLINE",
    .request_id = "",
    .tool = "",
    .summary = "",
    .decision = "",
};

esp_err_t kiro_passport_init(const char *device_id)
{
    (void)device_id;
    s_snapshot.initialized = true;
    strncpy(s_snapshot.state, "IDLE", sizeof(s_snapshot.state) - 1);
    return ESP_OK;
}

void kiro_passport_get_snapshot(kiro_passport_snapshot_t *snapshot)
{
    if (snapshot) *snapshot = s_snapshot;
}

void kiro_passport_set_connection(bool connected, const char *session_id)
{
    (void)session_id;
    s_snapshot.connected = connected;
}

kiro_passport_request_result_t kiro_passport_submit_request(
    const char *message, time_t now, kiro_passport_decision_t *rejection)
{
    (void)message;
    (void)now;
    (void)rejection;
    return KIRO_PASSPORT_REQUEST_DENIED_INVALID;
}

void kiro_passport_expire(time_t now)
{
    (void)now;
}

esp_err_t kiro_passport_decide(bool allow)
{
    (void)allow;
    return ESP_ERR_NOT_SUPPORTED;
}

bool kiro_passport_get_decision(kiro_passport_decision_t *decision)
{
    (void)decision;
    return false;
}

void kiro_passport_ack_decision(const kiro_passport_decision_t *decision)
{
    (void)decision;
}

/* ==================== kiro_passport_network ==================== */
esp_err_t kiro_passport_network_init(void)
{
    /* 无网络：Kiro 页在菜单中显示"不可用" */
    return ESP_ERR_NOT_SUPPORTED;
}

void kiro_passport_network_get_config(kiro_passport_network_config_t *config)
{
    if (config) memset(config, 0, sizeof(*config));
}

esp_err_t kiro_passport_network_configure(const char *relay_url, const char *credential)
{
    (void)relay_url;
    (void)credential;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t kiro_passport_network_clear_configuration(void)
{
    return ESP_OK;
}

bool kiro_passport_network_enrollment_supported(void)
{
    return false;
}

esp_err_t kiro_passport_network_start_enrollment(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void kiro_passport_network_cancel_enrollment(void)
{
}

void kiro_passport_network_get_enrollment(kiro_passport_enrollment_snapshot_t *snapshot)
{
    if (snapshot) {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->state = KIRO_PASSPORT_ENROLLMENT_IDLE;
        snapshot->last_error = ESP_OK;
    }
}

kiro_passport_network_state_t kiro_passport_network_get_state(void)
{
    return KIRO_PASSPORT_NETWORK_UNCONFIGURED;
}

const char *kiro_passport_network_state_name(kiro_passport_network_state_t state)
{
    switch (state) {
    case KIRO_PASSPORT_NETWORK_UNCONFIGURED: return "UNCONFIGURED";
    case KIRO_PASSPORT_NETWORK_WAITING_WIFI:  return "WAITING WIFI";
    case KIRO_PASSPORT_NETWORK_WAITING_CLOCK: return "WAITING CLOCK";
    case KIRO_PASSPORT_NETWORK_CONNECTING:    return "CONNECTING";
    case KIRO_PASSPORT_NETWORK_CONNECTED:     return "CONNECTED";
    case KIRO_PASSPORT_NETWORK_ERROR:         return "ERROR";
    default:                                  return "?";
    }
}

bool kiro_passport_network_get_image(kiro_passport_image_info_t *out_info)
{
    (void)out_info;
    return false; /* P0：无图片推送 */
}

int kiro_passport_network_send_text(const char *message)
{
    (void)message;
    return -1;
}

bool kiro_passport_network_is_connected(void)
{
    return false;
}
