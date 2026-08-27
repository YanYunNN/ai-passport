/* simulator/shell/shim_nvs.c
 * NVS 的内存版宿主实现（外壳侧），经 sim_api_t 提供给固件模块。
 *
 * 数据只保留在单次进程运行内：重启模拟器后 app_settings 回到默认值。
 * 仅实现 app_settings.c 用到的 blob 读写路径（open/get/set/commit/close）。
 */
#include "nvs.h"
#include "nvs_flash.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 内部存储结构 ---------------- */
#define SIM_NVS_MAX_NS   8
#define SIM_NVS_MAX_KEYS 16
#define SIM_NVS_MAX_BLOB (16 * 1024)

typedef struct {
    char key[NVS_KEY_NAME_MAX_SIZE];
    uint8_t *data;
    size_t size;
    bool used;
} sim_nvs_entry_t;

typedef struct {
    char name[NVS_NS_NAME_MAX_SIZE];
    sim_nvs_entry_t entries[SIM_NVS_MAX_KEYS];
    bool used;
} sim_nvs_ns_t;

static sim_nvs_ns_t s_ns[SIM_NVS_MAX_NS];
static bool s_initialized;

/* ---------------- nvs_flash ---------------- */
esp_err_t nvs_flash_init(void)
{
    s_initialized = true;
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void)
{
    memset(s_ns, 0, sizeof(s_ns));
    return ESP_OK;
}

/* ---------------- nvs ---------------- */
static sim_nvs_ns_t *find_ns(const char *namespace_name, bool create)
{
    for (size_t i = 0; i < SIM_NVS_MAX_NS; i++) {
        if (s_ns[i].used && strcmp(s_ns[i].name, namespace_name) == 0) {
            return &s_ns[i];
        }
    }
    if (!create) return NULL;
    for (size_t i = 0; i < SIM_NVS_MAX_NS; i++) {
        if (!s_ns[i].used) {
            strncpy(s_ns[i].name, namespace_name, NVS_NS_NAME_MAX_SIZE - 1);
            s_ns[i].used = true;
            return &s_ns[i];
        }
    }
    return NULL;
}

static sim_nvs_entry_t *find_entry(sim_nvs_ns_t *ns, const char *key, bool create)
{
    for (size_t i = 0; i < SIM_NVS_MAX_KEYS; i++) {
        if (ns->entries[i].used && strcmp(ns->entries[i].key, key) == 0) {
            return &ns->entries[i];
        }
    }
    if (!create) return NULL;
    for (size_t i = 0; i < SIM_NVS_MAX_KEYS; i++) {
        if (!ns->entries[i].used) {
            strncpy(ns->entries[i].key, key, NVS_KEY_NAME_MAX_SIZE - 1);
            ns->entries[i].used = true;
            return &ns->entries[i];
        }
    }
    return NULL;
}

esp_err_t nvs_open(const char *namespace_name, uint32_t open_mode,
                   nvs_handle_t *out_handle)
{
    (void)open_mode;
    if (!namespace_name || !out_handle) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_NVS_NOT_INITIALIZED;

    sim_nvs_ns_t *ns = find_ns(namespace_name, open_mode == NVS_READWRITE);
    if (!ns) return ESP_ERR_NVS_NOT_FOUND;

    /* 句柄直接复用命名空间指针（int 宽度足够） */
    *out_handle = (nvs_handle_t)(uintptr_t)ns;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key,
                       void *out_value, size_t *length)
{
    if (!key || !length) return ESP_ERR_INVALID_ARG;

    sim_nvs_ns_t *ns = (sim_nvs_ns_t *)(uintptr_t)handle;
    if (!ns || !ns->used) return ESP_ERR_NVS_NOT_INITIALIZED;

    sim_nvs_entry_t *entry = find_entry(ns, key, false);
    if (!entry) return ESP_ERR_NVS_NOT_FOUND;

    if (!out_value) {
        *length = entry->size;
        return ESP_OK;
    }
    if (*length < entry->size) {
        *length = entry->size;
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out_value, entry->data, entry->size);
    *length = entry->size;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                       const void *value, size_t length)
{
    if (!key || (!value && length > 0)) return ESP_ERR_INVALID_ARG;
    if (length > SIM_NVS_MAX_BLOB) return ESP_ERR_NVS_NOT_ENOUGH_SPACE;

    sim_nvs_ns_t *ns = (sim_nvs_ns_t *)(uintptr_t)handle;
    if (!ns || !ns->used) return ESP_ERR_NVS_NOT_INITIALIZED;

    sim_nvs_entry_t *entry = find_entry(ns, key, true);
    if (!entry) return ESP_ERR_NVS_NOT_ENOUGH_SPACE;

    uint8_t *data = NULL;
    if (length > 0) {
        data = (uint8_t *)malloc(length);
        if (!data) return ESP_ERR_NO_MEM;
        memcpy(data, value, length);
    }
    free(entry->data);
    entry->data = data;
    entry->size = length;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}
