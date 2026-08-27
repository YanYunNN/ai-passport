/* simulator/include/nvs.h
 * ESP-IDF NVS API 宿主垫片：内存版实现，见 simulator/src/shim_nvs.c。
 * 数据只保留在单次运行内（P0 简化），重启模拟器后设置回到默认值。
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t nvs_handle_t;

#define NVS_READONLY  0
#define NVS_READWRITE 1

#define NVS_KEY_NAME_MAX_SIZE 16
#define NVS_NS_NAME_MAX_SIZE 16

/* 打开命名空间。命名空间不存在时（无论模式）返回 ESP_ERR_NVS_NOT_FOUND。 */
esp_err_t nvs_open(const char *namespace_name, uint32_t open_mode,
                   nvs_handle_t *out_handle);

/* 读取 blob。out_value 为 NULL 时只回填实际长度（用于长度查询）。
 * 键不存在返回 ESP_ERR_NVS_NOT_FOUND。 */
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key,
                       void *out_value, size_t *length);

/* 写入 blob（覆盖已存在的键）。 */
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                       const void *value, size_t length);

/* 内存版无需持久化；为语义完整性保留，总是成功。 */
esp_err_t nvs_commit(nvs_handle_t handle);

void nvs_close(nvs_handle_t handle);

#ifdef __cplusplus
}
#endif
