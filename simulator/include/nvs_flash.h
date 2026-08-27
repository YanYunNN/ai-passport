/* simulator/include/nvs_flash.h
 * ESP-IDF nvs_flash 宿主垫片。 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化内存版 NVS 存储。总是成功。 */
esp_err_t nvs_flash_init(void);

/* 清空内存版 NVS 存储。 */
esp_err_t nvs_flash_erase(void);

#ifdef __cplusplus
}
#endif
