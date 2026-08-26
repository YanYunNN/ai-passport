#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化实时投屏模块与后台采集任务
 */
esp_err_t screencast_init(void);

/**
 * @brief 启用或关闭周期性实时投屏
 */
void screencast_set_enabled(bool enabled);

/**
 * @brief 查询当前是否开启实时投屏
 */
bool screencast_is_enabled(void);

/**
 * @brief 请求立即捕获一帧全屏并上报（远程截屏）
 */
void screencast_request_capture(void);

/**
 * @brief 设置实时投屏帧间隔（毫秒），默认 1000ms
 */
void screencast_set_interval_ms(uint32_t interval_ms);

#ifdef __cplusplus
}
#endif
