/* simulator/shell/shim_esp_log.c
 * esp_log / esp_err 的宿主实现（外壳侧），通过 sim_api_t 暴露给固件模块。
 *
 * 与 IDF esp_log 相同的调用模式（见 IDF components/log/src/log_format_text.c）：
 * 日志前缀（"I (123) tag: "）、正文、换行是三次独立的 vprintf 调用，
 * 每次都向当前 vprintf 函数传入【真正的 va_list】（由各自函数内 va_start 产生）。
 * 绝不能把字符串当 va_list 传（GCC x64 上 va_list 就是 char*，会崩溃）。
 *
 * debug_log.c（固件模块侧）初始化后通过 esp_log_set_vprintf() 挂接捕获函数，
 * 因此日志同时出现在控制台和"设置 → 日志查看"页。
 */
#include "esp_log.h"
#include "esp_err.h"
#include "sim_api.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static vprintf_like_t s_vprintf = NULL;

static int default_vprintf(const char *fmt, va_list args)
{
    return vprintf(fmt, args);
}

static uint32_t uptime_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* 自带 va_list 的一次 vprintf（前缀/换行等固定内容用）。 */
static void emit(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf_like_t fn = s_vprintf ? s_vprintf : default_vprintf;
    fn(fmt, args);
    va_end(args);
}

vprintf_like_t esp_log_set_vprintf(vprintf_like_t func)
{
    vprintf_like_t previous = s_vprintf;
    s_vprintf = func ? func : default_vprintf;
    return previous;
}

/* 服务表入口：固件模块的 esp_log_printf 胶水最终转调到这里。 */
void esp_log_vprintf(int level, const char *tag, const char *fmt, va_list args)
{
    /* 前缀：独立调用，构造自己的 va_list（与 IDF log_format_text.c 一致） */
    emit("%c (%u) %s: ", esp_log_level_name(level)[0], (unsigned)uptime_ms(),
         tag ? tag : "");

    /* 正文：把调用者真正的 va_list 原样传给 vprintf 函数 */
    vprintf_like_t fn = s_vprintf ? s_vprintf : default_vprintf;
    fn(fmt, args);

    emit("\n");
}

/* 外壳自身（含 autotest）使用的变参封装 */
void esp_log_printf(int level, const char *tag, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    esp_log_vprintf(level, tag, fmt, args);
    va_end(args);
}

const char *esp_log_level_name(int level)
{
    switch (level) {
    case ESP_LOG_ERROR:   return "E";
    case ESP_LOG_WARN:    return "W";
    case ESP_LOG_INFO:    return "I";
    case ESP_LOG_DEBUG:   return "D";
    case ESP_LOG_VERBOSE: return "V";
    default:              return "?";
    }
}

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK:                    return "ESP_OK";
    case ESP_FAIL:                  return "ESP_FAIL";
    case ESP_ERR_NO_MEM:            return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:       return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:     return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:      return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:         return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED:     return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT:           return "ESP_ERR_TIMEOUT";
    case ESP_ERR_INVALID_RESPONSE:  return "ESP_ERR_INVALID_RESPONSE";
    case ESP_ERR_NVS_NOT_INITIALIZED: return "ESP_ERR_NVS_NOT_INITIALIZED";
    case ESP_ERR_NVS_NOT_FOUND:     return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_NVS_TYPE_MISMATCH: return "ESP_ERR_NVS_TYPE_MISMATCH";
    case ESP_ERR_NVS_READ_ONLY:     return "ESP_ERR_NVS_READ_ONLY";
    case ESP_ERR_NVS_NOT_ENOUGH_SPACE: return "ESP_ERR_NVS_NOT_ENOUGH_SPACE";
    default:                        return "ESP_ERR_UNKNOWN";
    }
}
