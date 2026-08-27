/* simulator/include/esp_log.h
 * ESP-IDF 日志的宿主垫片：ESP_LOGx 格式化后经当前 vprintf 链输出，
 * 因此 debug_log.c 的 esp_log_set_vprintf 捕获机制原样工作。
 */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h> /* IDF 的 esp_log.h 也传递引入 stdio，宿主对齐 */

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_LOG_ERROR   1
#define ESP_LOG_WARN    2
#define ESP_LOG_INFO    3
#define ESP_LOG_DEBUG   4
#define ESP_LOG_VERBOSE 5

/* 与 IDF esp_log.h 一致：vprintf 风格的日志输出函数 */
typedef int (*vprintf_like_t)(const char *fmt, va_list args);

/* 设置/替换日志输出函数，返回上一个函数指针（可能为 NULL）。 */
vprintf_like_t esp_log_set_vprintf(vprintf_like_t func);

/* 格式串检查：gnu_printf 是 GCC/clang 通用格式表，支持 %zu 等 C99 修饰符 */
void esp_log_printf(int level, const char *tag, const char *fmt, ...)
    __attribute__((format(gnu_printf, 3, 4)));

/* 服务表入口（外壳侧实现）：与 esp_log_printf 等价，但接收现成的 va_list。
 * 固件模块的 esp_log_printf 胶水最终转调到这里；模块自身不要直接调用。 */
void esp_log_vprintf(int level, const char *tag, const char *fmt, va_list args);

#define ESP_LOGE(tag, ...) esp_log_printf(ESP_LOG_ERROR, tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) esp_log_printf(ESP_LOG_WARN,  tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) esp_log_printf(ESP_LOG_INFO,  tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) esp_log_printf(ESP_LOG_DEBUG, tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) esp_log_printf(ESP_LOG_VERBOSE, tag, __VA_ARGS__)

/* 启动早期日志：宿主侧与 ESP_LOGx 等价 */
#define ESP_EARLY_LOGE(tag, ...) ESP_LOGE(tag, __VA_ARGS__)
#define ESP_EARLY_LOGW(tag, ...) ESP_LOGW(tag, __VA_ARGS__)
#define ESP_EARLY_LOGI(tag, ...) ESP_LOGI(tag, __VA_ARGS__)
#define ESP_EARLY_LOGD(tag, ...) ESP_LOGD(tag, __VA_ARGS__)

/* 日志级别名称，如 "E"/"W"/"I"（与 IDF 控制台前缀一致） */
const char *esp_log_level_name(int level);

#ifdef __cplusplus
}
#endif
