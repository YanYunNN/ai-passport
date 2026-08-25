#include "app_settings.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdbool.h>

#define APP_SETTINGS_NAMESPACE "appcfg"
#define APP_SETTINGS_KEY "cfg"
#define APP_SETTINGS_VERSION_V1 1
#define APP_SETTINGS_VERSION_V2 2
#define APP_SETTINGS_VERSION 3

static const char *TAG = "app_settings";

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t brightness_index;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t time_format;
} app_settings_record_v1_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t brightness_index;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t time_format;
    uint8_t wifi_enabled;
} app_settings_record_v2_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t brightness_index;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t time_format;
    uint8_t wifi_enabled;
    uint8_t light_sleep_enabled;
    uint8_t wifi_power_save_enabled;
} app_settings_record_t;

_Static_assert(sizeof(app_settings_record_v1_t) == 6, "v1 settings layout changed");
_Static_assert(sizeof(app_settings_record_v2_t) == 7, "v2 settings layout changed");
_Static_assert(sizeof(app_settings_record_t) == 9, "v3 settings layout changed");

static const app_settings_t s_defaults = {
    .brightness_index = 9,
    .hour = 0,
    .minute = 0,
    .second = 0,
    .time_format = APP_SETTINGS_TIME_HH_MM,
    .wifi_enabled = true,
    .light_sleep_enabled = true,
    .wifi_power_save_enabled = true,
};

static app_settings_t s_settings;
static bool s_initialized;

static bool settings_valid(const app_settings_t *settings)
{
    return settings && settings->brightness_index <= 9 && settings->hour < 24 &&
           settings->minute < 60 && settings->second < 60 &&
           (settings->time_format == APP_SETTINGS_TIME_HH_MM ||
            settings->time_format == APP_SETTINGS_TIME_HH_MM_SS);
}

static app_settings_record_t record_from_settings(const app_settings_t *settings)
{
    return (app_settings_record_t){
        .version = APP_SETTINGS_VERSION,
        .brightness_index = settings->brightness_index,
        .hour = settings->hour,
        .minute = settings->minute,
        .second = settings->second,
        .time_format = (uint8_t)settings->time_format,
        .wifi_enabled = settings->wifi_enabled ? 1u : 0u,
        .light_sleep_enabled = settings->light_sleep_enabled ? 1u : 0u,
        .wifi_power_save_enabled = settings->wifi_power_save_enabled ? 1u : 0u,
    };
}

static bool settings_from_v1_record(const app_settings_record_v1_t *record,
                                    app_settings_t *settings)
{
    if (!record || !settings || record->version != APP_SETTINGS_VERSION_V1) return false;

    *settings = (app_settings_t){
        .brightness_index = record->brightness_index,
        .hour = record->hour,
        .minute = record->minute,
        .second = record->second,
        .time_format = (app_settings_time_format_t)record->time_format,
        .wifi_enabled = true,
        .light_sleep_enabled = true,
        .wifi_power_save_enabled = true,
    };
    return settings_valid(settings);
}

static bool settings_from_v2_record(const app_settings_record_v2_t *record,
                                    app_settings_t *settings)
{
    if (!record || !settings || record->version != APP_SETTINGS_VERSION_V2 ||
        record->wifi_enabled > 1u) {
        return false;
    }

    *settings = (app_settings_t){
        .brightness_index = record->brightness_index,
        .hour = record->hour,
        .minute = record->minute,
        .second = record->second,
        .time_format = (app_settings_time_format_t)record->time_format,
        .wifi_enabled = record->wifi_enabled != 0u,
        .light_sleep_enabled = true,
        .wifi_power_save_enabled = true,
    };
    return settings_valid(settings);
}

static bool settings_from_record(const app_settings_record_t *record,
                                 app_settings_t *settings)
{
    if (!record || !settings || record->version != APP_SETTINGS_VERSION ||
        record->wifi_enabled > 1u || record->light_sleep_enabled > 1u ||
        record->wifi_power_save_enabled > 1u) {
        return false;
    }

    *settings = (app_settings_t){
        .brightness_index = record->brightness_index,
        .hour = record->hour,
        .minute = record->minute,
        .second = record->second,
        .time_format = (app_settings_time_format_t)record->time_format,
        .wifi_enabled = record->wifi_enabled != 0u,
        .light_sleep_enabled = record->light_sleep_enabled != 0u,
        .wifi_power_save_enabled = record->wifi_power_save_enabled != 0u,
    };
    return settings_valid(settings);
}

static esp_err_t save_settings_record(const app_settings_t *settings)
{
    app_settings_record_t record = record_from_settings(settings);
    nvs_handle_t handle;
    esp_err_t result = nvs_open(APP_SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;

    result = nvs_set_blob(handle, APP_SETTINGS_KEY, &record, sizeof(record));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

static esp_err_t load_settings(void)
{
    s_settings = s_defaults;

    nvs_handle_t handle;
    esp_err_t result = nvs_open(APP_SETTINGS_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;

    size_t size = 0;
    result = nvs_get_blob(handle, APP_SETTINGS_KEY, NULL, &size);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (result != ESP_OK) {
        nvs_close(handle);
        return result;
    }

    uint8_t migrated_from = 0;
    if (size == sizeof(app_settings_record_v1_t)) {
        app_settings_record_v1_t record;
        result = nvs_get_blob(handle, APP_SETTINGS_KEY, &record, &size);
        if (result == ESP_OK && settings_from_v1_record(&record, &s_settings)) {
            migrated_from = APP_SETTINGS_VERSION_V1;
        } else {
            s_settings = s_defaults;
        }
    } else if (size == sizeof(app_settings_record_v2_t)) {
        app_settings_record_v2_t record;
        result = nvs_get_blob(handle, APP_SETTINGS_KEY, &record, &size);
        if (result == ESP_OK && settings_from_v2_record(&record, &s_settings)) {
            migrated_from = APP_SETTINGS_VERSION_V2;
        } else {
            s_settings = s_defaults;
        }
    } else if (size == sizeof(app_settings_record_t)) {
        app_settings_record_t record;
        result = nvs_get_blob(handle, APP_SETTINGS_KEY, &record, &size);
        if (result != ESP_OK || !settings_from_record(&record, &s_settings)) {
            s_settings = s_defaults;
        }
    } else {
        s_settings = s_defaults;
    }
    nvs_close(handle);

    if (result != ESP_OK) return result;
    if (migrated_from == 0) return ESP_OK;

    result = save_settings_record(&s_settings);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "已从 v%u 迁移应用设置到 v%u", migrated_from,
                 APP_SETTINGS_VERSION);
    } else {
        ESP_LOGW(TAG, "v%u 设置已载入，但迁移尚未保存: %s", migrated_from,
                 esp_err_to_name(result));
    }
    /* Valid older settings remain usable even if the one-time rewrite failed. */
    return ESP_OK;
}

esp_err_t app_settings_init(void)
{
    if (s_initialized) return ESP_OK;

    esp_err_t result = nvs_flash_init();
    if (result != ESP_OK) {
        s_settings = s_defaults;
        return result;
    }

    result = load_settings();
    if (result != ESP_OK) {
        s_settings = s_defaults;
        return result;
    }
    s_initialized = true;
    return ESP_OK;
}

const app_settings_t *app_settings_get(void)
{
    return &s_settings;
}

uint8_t app_settings_get_brightness_percent(void)
{
    static const uint8_t levels[] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
    if (s_settings.brightness_index >= sizeof(levels) / sizeof(levels[0])) {
        return 100;
    }
    return levels[s_settings.brightness_index];
}

esp_err_t app_settings_save(const app_settings_t *settings)
{
    if (!settings_valid(settings)) return ESP_ERR_INVALID_ARG;

    esp_err_t result = save_settings_record(settings);
    if (result == ESP_OK) s_settings = *settings;
    return result;
}

esp_err_t app_settings_reset(void)
{
    return app_settings_save(&s_defaults);
}
