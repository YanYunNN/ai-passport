#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEBUG_LOG_MAX_LINES 36
#define DEBUG_LOG_LINE_MAX_LEN 96

typedef enum {
    DEBUG_LOG_TYPE_DEVICE = 0,
    DEBUG_LOG_TYPE_NETWORK = 1,
} debug_log_type_t;

/* Initialize log hook via esp_log_set_vprintf */
esp_err_t debug_log_init(bool enabled);

/* Enable / disable log capture */
void debug_log_set_enabled(bool enabled);
bool debug_log_is_enabled(void);

/* Get snapshot of log lines for given type.
 * Returns the total number of lines filled.
 */
size_t debug_log_get_lines(debug_log_type_t type, char lines_out[][DEBUG_LOG_LINE_MAX_LEN], size_t max_lines);

/* Clear stored logs for a given type */
void debug_log_clear(debug_log_type_t type);
