#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化按需远程截屏模块
 */
esp_err_t screencast_init(void);

/**
 * @brief 触发单次远程截屏并上报
 */
void screencast_request_capture(void);

/**
 * @brief 设置投屏启用状态
 */
void screencast_set_enabled(bool enabled);

/**
 * @brief 获取投屏启用状态
 */
bool screencast_is_enabled(void);

#ifdef __cplusplus
}
#endif
