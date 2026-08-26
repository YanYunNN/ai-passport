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
    bool debug_enabled;
    uint8_t screen_timeout_index;
    uint8_t auto_sleep_timeout_index;
} app_settings_t;

#define APP_SETTINGS_SCREEN_TIMEOUT_COUNT 6
#define APP_SETTINGS_AUTO_SLEEP_COUNT 5

/* Initializes default NVS and loads the app-owned appcfg/cfg record. */
esp_err_t app_settings_init(void);
const app_settings_t *app_settings_get(void);
uint8_t app_settings_get_brightness_percent(void);
uint16_t app_settings_get_screen_timeout_seconds(void);
uint16_t app_settings_get_auto_sleep_timeout_seconds(void);
const char *app_settings_get_screen_timeout_text(uint8_t index);
const char *app_settings_get_auto_sleep_timeout_text(uint8_t index);

/* Saves a fully validated configuration as one committed NVS record. */
esp_err_t app_settings_save(const app_settings_t *settings);
esp_err_t app_settings_reset(void);
