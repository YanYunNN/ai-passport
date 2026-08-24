#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    APP_SETTINGS_TIME_HH_MM,
    APP_SETTINGS_TIME_HH_MM_SS,
} app_settings_time_format_t;

typedef struct {
    uint8_t brightness_index;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    app_settings_time_format_t time_format;
    bool wifi_enabled;
    bool light_sleep_enabled;
    bool wifi_power_save_enabled;
} app_settings_t;

/* Initializes default NVS and loads the app-owned appcfg/cfg record. */
esp_err_t app_settings_init(void);
const app_settings_t *app_settings_get(void);
uint8_t app_settings_get_brightness_percent(void);

/* Saves a fully validated configuration as one committed NVS record. */
esp_err_t app_settings_save(const app_settings_t *settings);
esp_err_t app_settings_reset(void);
