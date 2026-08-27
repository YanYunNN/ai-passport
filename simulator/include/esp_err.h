/* simulator/include/esp_err.h
 * ESP-IDF esp_err 的宿主垫片：只提供 main/ 实际用到的错误码。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t esp_err_t;

#define ESP_OK                  0
#define ESP_FAIL                (-1)
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_INVALID_SIZE    0x104
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_NOT_SUPPORTED   0x106
#define ESP_ERR_TIMEOUT         0x107
#define ESP_ERR_INVALID_RESPONSE 0x10c
#define ESP_ERR_INVALID_CRC     0x111
#define ESP_ERR_NOT_ALLOWED     0x10d

/* NVS 错误码（app_settings.c 依赖 ESP_ERR_NVS_NOT_FOUND） */
#define ESP_ERR_NVS_BASE                 0x1100
#define ESP_ERR_NVS_NOT_INITIALIZED      (ESP_ERR_NVS_BASE + 0x01)
#define ESP_ERR_NVS_NOT_FOUND            (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_TYPE_MISMATCH        (ESP_ERR_NVS_BASE + 0x03)
#define ESP_ERR_NVS_READ_ONLY            (ESP_ERR_NVS_BASE + 0x04)
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE     (ESP_ERR_NVS_BASE + 0x05)

const char *esp_err_to_name(esp_err_t code);

#ifdef __cplusplus
}
#endif
