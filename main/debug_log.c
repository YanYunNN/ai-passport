#include "debug_log.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static bool s_enabled = true;
static bool s_initialized = false;
static vprintf_like_t s_default_vprintf = NULL;

static char s_device_lines[DEBUG_LOG_MAX_LINES][DEBUG_LOG_LINE_MAX_LEN];
static size_t s_device_head = 0;
static size_t s_device_count = 0;

static char s_network_lines[DEBUG_LOG_MAX_LINES][DEBUG_LOG_LINE_MAX_LEN];
static size_t s_network_head = 0;
static size_t s_network_count = 0;

static portMUX_TYPE s_log_lock = portMUX_INITIALIZER_UNLOCKED;

static bool is_network_log(const char *str)
{
    if (!str) return false;
    /* Case-insensitive substrings related to WebSocket long connection & Relay */
    static const char * const keywords[] = {
        "passport_wss",
        "websocket",
        "WEBSOCKET",
        "relay",
        "Relay",
        "RELAY",
        "kiro_passport",
        "transport_ws",
        "esp-tls",
    };
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (strstr(str, keywords[i]) != NULL) {
            return true;
        }
    }
    return false;
}

static void strip_ansi_and_append(debug_log_type_t type, const char *raw_msg)
{
    if (!raw_msg || !*raw_msg) return;

    char cleaned[DEBUG_LOG_LINE_MAX_LEN];
    size_t out_idx = 0;
    const char *p = raw_msg;

    while (*p && out_idx + 1 < sizeof(cleaned)) {
        if (*p == '\033' && *(p + 1) == '[') {
            /* Skip ANSI escape code */
            p += 2;
            while (*p && *p != 'm') {
                p++;
            }
            if (*p == 'm') p++;
            continue;
        }
        if (*p == '\r' || *p == '\n') {
            p++;
            continue;
        }
        cleaned[out_idx++] = *p++;
    }
    cleaned[out_idx] = '\0';

    if (out_idx == 0) return;

    taskENTER_CRITICAL(&s_log_lock);
    if (type == DEBUG_LOG_TYPE_NETWORK) {
        snprintf(s_network_lines[s_network_head], DEBUG_LOG_LINE_MAX_LEN, "%s", cleaned);
        s_network_head = (s_network_head + 1) % DEBUG_LOG_MAX_LINES;
        if (s_network_count < DEBUG_LOG_MAX_LINES) {
            s_network_count++;
        }
    } else {
        snprintf(s_device_lines[s_device_head], DEBUG_LOG_LINE_MAX_LEN, "%s", cleaned);
        s_device_head = (s_device_head + 1) % DEBUG_LOG_MAX_LINES;
        if (s_device_count < DEBUG_LOG_MAX_LINES) {
            s_device_count++;
        }
    }
    taskEXIT_CRITICAL(&s_log_lock);
}

static int custom_vprintf(const char *fmt, va_list args)
{
    va_list args_copy;
    va_copy(args_copy, args);

    /* Output to default console */
    int ret = 0;
    if (s_default_vprintf) {
        ret = s_default_vprintf(fmt, args);
    } else {
        ret = vprintf(fmt, args);
    }

    if (s_enabled) {
        char buffer[192];
        int len = vsnprintf(buffer, sizeof(buffer), fmt, args_copy);
        if (len > 0) {
            debug_log_type_t type = is_network_log(buffer) ? DEBUG_LOG_TYPE_NETWORK : DEBUG_LOG_TYPE_DEVICE;
            strip_ansi_and_append(type, buffer);
        }
    }
    va_end(args_copy);
    return ret;
}

esp_err_t debug_log_init(bool enabled)
{
    s_enabled = enabled;
    if (s_initialized) return ESP_OK;

    s_default_vprintf = esp_log_set_vprintf(custom_vprintf);
    s_initialized = true;
    return ESP_OK;
}

void debug_log_set_enabled(bool enabled)
{
    s_enabled = enabled;
}

bool debug_log_is_enabled(void)
{
    return s_enabled;
}

size_t debug_log_get_lines(debug_log_type_t type, char lines_out[][DEBUG_LOG_LINE_MAX_LEN], size_t max_lines)
{
    if (!lines_out || max_lines == 0) return 0;

    size_t copied = 0;
    taskENTER_CRITICAL(&s_log_lock);
    size_t count = (type == DEBUG_LOG_TYPE_NETWORK) ? s_network_count : s_device_count;
    size_t head = (type == DEBUG_LOG_TYPE_NETWORK) ? s_network_head : s_device_head;

    if (count > max_lines) count = max_lines;
    size_t start = (head + DEBUG_LOG_MAX_LINES - count) % DEBUG_LOG_MAX_LINES;

    for (size_t i = 0; i < count; i++) {
        size_t idx = (start + i) % DEBUG_LOG_MAX_LINES;
        if (type == DEBUG_LOG_TYPE_NETWORK) {
            snprintf(lines_out[i], DEBUG_LOG_LINE_MAX_LEN, "%s", s_network_lines[idx]);
        } else {
            snprintf(lines_out[i], DEBUG_LOG_LINE_MAX_LEN, "%s", s_device_lines[idx]);
        }
        copied++;
    }
    taskEXIT_CRITICAL(&s_log_lock);
    return copied;
}

void debug_log_clear(debug_log_type_t type)
{
    taskENTER_CRITICAL(&s_log_lock);
    if (type == DEBUG_LOG_TYPE_NETWORK) {
        s_network_head = 0;
        s_network_count = 0;
    } else {
        s_device_head = 0;
        s_device_count = 0;
    }
    taskEXIT_CRITICAL(&s_log_lock);
}
