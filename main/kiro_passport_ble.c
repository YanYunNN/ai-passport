#include "kiro_passport_ble.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "kiro_ble";

#define KIRO_SERVICE_UUID \
    BLE_UUID128_INIT(0x01, 0x19, 0xc1, 0x73, 0x8d, 0x7f, 0x17, 0x8d, \
                     0x4a, 0x4f, 0x2c, 0x5a, 0x01, 0x00, 0xaa, 0x3e)
#define KIRO_COMMAND_UUID \
    BLE_UUID128_INIT(0x02, 0x19, 0xc1, 0x73, 0x8d, 0x7f, 0x17, 0x8d, \
                     0x4a, 0x4f, 0x2c, 0x5a, 0x01, 0x00, 0xaa, 0x3e)
#define KIRO_STATUS_UUID \
    BLE_UUID128_INIT(0x03, 0x19, 0xc1, 0x73, 0x8d, 0x7f, 0x17, 0x8d, \
                     0x4a, 0x4f, 0x2c, 0x5a, 0x01, 0x00, 0xaa, 0x3e)

#define KIRO_MAX_MESSAGE 224
#define KIRO_INVALID_CONN_HANDLE 0xffff

static SemaphoreHandle_t s_mutex;
static kiro_passport_snapshot_t s_snapshot;
static uint16_t s_connection_handle = KIRO_INVALID_CONN_HANDLE;
static uint16_t s_command_handle;
static uint16_t s_status_handle;
static uint8_t s_own_addr_type;

static const ble_uuid128_t s_service_uuid = KIRO_SERVICE_UUID;
static const ble_uuid128_t s_command_uuid = KIRO_COMMAND_UUID;
static const ble_uuid128_t s_status_uuid = KIRO_STATUS_UUID;

static void snapshot_lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void snapshot_unlock(void)
{
    xSemaphoreGive(s_mutex);
}

static void snapshot_reset_request_locked(void)
{
    s_snapshot.pending = false;
    s_snapshot.request_id[0] = '\0';
    s_snapshot.tool[0] = '\0';
    s_snapshot.summary[0] = '\0';
}

static bool is_safe_text(const char *text)
{
    for (; *text; text++) {
        if ((unsigned char)*text < 0x20 || *text == '"' || *text == '\\') return false;
    }
    return true;
}

static bool json_string(const char *json, const char *key, char *out, size_t out_size)
{
    char needle[32];
    int needle_len = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (needle_len <= 0 || (size_t)needle_len >= sizeof(needle)) return false;

    const char *value = strstr(json, needle);
    if (!value) return false;
    value += needle_len;
    while (isspace((unsigned char)*value)) value++;
    if (*value++ != ':') return false;
    while (isspace((unsigned char)*value)) value++;
    if (*value++ != '"') return false;

    size_t length = 0;
    while (*value && *value != '"') {
        if (*value == '\\' || (unsigned char)*value < 0x20 || length + 1 >= out_size) {
            return false;
        }
        out[length++] = *value++;
    }
    if (*value != '"') return false;
    out[length] = '\0';
    return is_safe_text(out);
}

static bool json_version_is_one(const char *json)
{
    const char *value = strstr(json, "\"v\"");
    if (!value) return false;
    value += 3;
    while (isspace((unsigned char)*value)) value++;
    if (*value++ != ':') return false;
    while (isspace((unsigned char)*value)) value++;
    return strtol(value, NULL, 10) == 1;
}

static bool valid_state(const char *state)
{
    return strcmp(state, "idle") == 0 || strcmp(state, "busy") == 0 ||
           strcmp(state, "sleep") == 0 || strcmp(state, "error") == 0;
}

static bool apply_command(const char *message)
{
    char type[12];
    if (!json_version_is_one(message) || !json_string(message, "type", type, sizeof(type))) {
        return false;
    }

    snapshot_lock();
    bool changed = false;
    if (strcmp(type, "state") == 0) {
        char state[sizeof(s_snapshot.state)];
        if (json_string(message, "state", state, sizeof(state)) && valid_state(state) &&
            !s_snapshot.pending) {
            snprintf(s_snapshot.state, sizeof(s_snapshot.state), "%s", state);
            s_snapshot.decision[0] = '\0';
            changed = true;
        }
    } else if (strcmp(type, "request") == 0) {
        char id[sizeof(s_snapshot.request_id)];
        char tool[sizeof(s_snapshot.tool)];
        char summary[sizeof(s_snapshot.summary)];
        if (json_string(message, "id", id, sizeof(id)) && id[0] &&
            json_string(message, "tool", tool, sizeof(tool)) && tool[0] &&
            json_string(message, "summary", summary, sizeof(summary))) {
            s_snapshot.pending = true;
            snprintf(s_snapshot.state, sizeof(s_snapshot.state), "%s", "attention");
            snprintf(s_snapshot.request_id, sizeof(s_snapshot.request_id), "%s", id);
            snprintf(s_snapshot.tool, sizeof(s_snapshot.tool), "%s", tool);
            snprintf(s_snapshot.summary, sizeof(s_snapshot.summary), "%s", summary);
            s_snapshot.decision[0] = '\0';
            changed = true;
        }
    } else if (strcmp(type, "clear") == 0) {
        char id[sizeof(s_snapshot.request_id)];
        if (json_string(message, "id", id, sizeof(id)) && id[0] &&
            strcmp(id, s_snapshot.request_id) == 0) {
            snapshot_reset_request_locked();
            s_snapshot.decision[0] = '\0';
            snprintf(s_snapshot.state, sizeof(s_snapshot.state), "%s", "idle");
            changed = true;
        }
    }
    snapshot_unlock();
    return changed;
}

static int encode_status(char *buffer, size_t buffer_size)
{
    kiro_passport_snapshot_t snapshot;
    kiro_passport_ble_get_snapshot(&snapshot);
    return snprintf(buffer, buffer_size,
                    "{\"v\":1,\"type\":\"status\",\"state\":\"%s\","
                    "\"connected\":%s,\"encrypted\":%s,\"pending\":%s,"
                    "\"id\":\"%s\",\"tool\":\"%s\",\"summary\":\"%s\","
                    "\"decision\":\"%s\"}",
                    snapshot.state, snapshot.connected ? "true" : "false",
                    snapshot.encrypted ? "true" : "false",
                    snapshot.pending ? "true" : "false", snapshot.request_id,
                    snapshot.tool, snapshot.summary, snapshot.decision);
}

static void notify_status(void)
{
    if (!s_status_handle) return;

    kiro_passport_snapshot_t snapshot;
    kiro_passport_ble_get_snapshot(&snapshot);
    if (!snapshot.connected || !snapshot.subscribed ||
        s_connection_handle == KIRO_INVALID_CONN_HANDLE) return;

    char message[KIRO_MAX_MESSAGE];
    int length = encode_status(message, sizeof(message));
    if (length <= 0 || length >= (int)sizeof(message)) return;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(message, (uint16_t)length);
    if (!om) {
        ESP_LOGW(TAG, "状态通知内存不足");
        return;
    }
    int rc = ble_gatts_notify_custom(s_connection_handle, s_status_handle, om);
    if (rc != 0) ESP_LOGW(TAG, "状态通知失败: %d", rc);
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (attr_handle == s_status_handle && ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        char message[KIRO_MAX_MESSAGE];
        int length = encode_status(message, sizeof(message));
        if (length <= 0 || length >= (int)sizeof(message)) return BLE_ATT_ERR_UNLIKELY;
        return os_mbuf_append(ctxt->om, message, (uint16_t)length) == 0
                   ? 0
                   : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (attr_handle != s_command_handle || ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
    if (length == 0 || length >= KIRO_MAX_MESSAGE) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    char message[KIRO_MAX_MESSAGE];
    uint16_t copied = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, message, sizeof(message) - 1, &copied);
    if (rc != 0 || copied != length) return BLE_ATT_ERR_UNLIKELY;
    message[length] = '\0';
    if (!apply_command(message)) return BLE_ATT_ERR_UNLIKELY;

    notify_status();
    return 0;
}

static const struct ble_gatt_svc_def s_gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_command_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
                .val_handle = &s_command_handle,
            },
            {
                .uuid = &s_status_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                         BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_status_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

static int gap_event(struct ble_gap_event *event, void *arg);

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    static const uint8_t advertisement_name[] = "KiroPass";
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = advertisement_name;
    fields.name_len = sizeof(advertisement_name) - 1;
    fields.name_is_complete = 1;
    fields.uuids128 = &s_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置广播数据失败: %d", rc);
        return;
    }

    struct ble_gap_adv_params parameters = { 0 };
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &parameters,
                           gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "启动广播失败: %d", rc);
        return;
    }

    snapshot_lock();
    s_snapshot.advertising = true;
    snapshot_unlock();
    ESP_LOGI(TAG, "Kiro Passport 广播已启动");
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        snapshot_lock();
        s_snapshot.advertising = false;
        if (event->connect.status == 0) {
            s_connection_handle = event->connect.conn_handle;
            s_snapshot.connected = true;
            s_snapshot.encrypted = false;
            s_snapshot.subscribed = false;
            ESP_LOGI(TAG, "BLE 已连接");
        }
        snapshot_unlock();
        if (event->connect.status != 0) start_advertising();
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        snapshot_lock();
        s_connection_handle = KIRO_INVALID_CONN_HANDLE;
        s_snapshot.connected = false;
        s_snapshot.encrypted = false;
        s_snapshot.subscribed = false;
        snapshot_reset_request_locked();
        snprintf(s_snapshot.state, sizeof(s_snapshot.state), "%s", "sleep");
        snapshot_unlock();
        ESP_LOGI(TAG, "BLE 已断开");
        start_advertising();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_status_handle) {
            snapshot_lock();
            s_snapshot.subscribed = event->subscribe.cur_notify;
            snapshot_unlock();
            if (event->subscribe.cur_notify) notify_status();
        }
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            snapshot_lock();
            s_snapshot.encrypted = true;
            snapshot_unlock();
            notify_status();
        }
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        return 0;
    default:
        return 0;
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE 地址不可用: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE 地址类型推断失败: %d", rc);
        return;
    }
    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE 重置: %d", reason);
}

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t kiro_passport_ble_init(void)
{
    if (s_mutex) return ESP_OK;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    memset(&s_snapshot, 0, sizeof(s_snapshot));
    snprintf(s_snapshot.state, sizeof(s_snapshot.state), "%s", "sleep");
    s_connection_handle = KIRO_INVALID_CONN_HANDLE;

    int rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "NimBLE 初始化失败: %d", rc);
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_FAIL;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;

    rc = ble_svc_gap_device_name_set("Kiro Passport");
    if (rc != 0) {
        ESP_LOGE(TAG, "设置 GAP 名称失败: %d", rc);
        return ESP_FAIL;
    }
    ble_svc_gap_init();
    ble_svc_gatt_init();
    rc = ble_gatts_count_cfg(s_gatt_services);
    if (rc == 0) rc = ble_gatts_add_svcs(s_gatt_services);
    if (rc != 0) {
        ESP_LOGE(TAG, "注册 GATT 服务失败: %d", rc);
        return ESP_FAIL;
    }

    snapshot_lock();
    s_snapshot.initialized = true;
    snapshot_unlock();
    nimble_port_freertos_init(nimble_host_task);
    return ESP_OK;
}

void kiro_passport_ble_get_snapshot(kiro_passport_snapshot_t *snapshot)
{
    if (!snapshot) return;
    if (!s_mutex) {
        memset(snapshot, 0, sizeof(*snapshot));
        return;
    }
    snapshot_lock();
    *snapshot = s_snapshot;
    snapshot_unlock();
}

esp_err_t kiro_passport_ble_decide(bool allow)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;

    snapshot_lock();
    if (!s_snapshot.pending) {
        snapshot_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(s_snapshot.decision, sizeof(s_snapshot.decision), "%s", allow ? "allow" : "deny");
    s_snapshot.pending = false;
    s_snapshot.tool[0] = '\0';
    s_snapshot.summary[0] = '\0';
    snprintf(s_snapshot.state, sizeof(s_snapshot.state), "%s", "idle");
    snapshot_unlock();
    notify_status();
    return ESP_OK;
}
