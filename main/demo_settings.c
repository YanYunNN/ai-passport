#include "app_settings.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "debug_log.h"
#include "game_audio.h"
#include "kiro_passport_network.h"
#include "power_manager.h"
#include "screencast.h"
#include "time_sync.h"
#include "ui_font_noto_sc_14.h"
#include "ui_font_noto_sc_20.h"
#include "ui_status.h"
#include "ui_system.h"
#include "wifi_manager.h"
#include <string.h>
#include "lvgl.h"

#define SETTING_COUNT 7
#define TIME_ACTION_COUNT 6
#define POWER_ACTION_COUNT 6
#define WIFI_ACTION_COUNT 4
#define NET_ROW_MAX (MAX_WIFI_PROFILES + 1)
#define RELAY_ACTION_COUNT 3
#define DEBUG_ACTION_COUNT 5

typedef enum {
    SETTING_BRIGHTNESS,
    SETTING_VOLUME,
    SETTING_TIME,
    SETTING_POWER,
    SETTING_WIFI,
    SETTING_RELAY,
    SETTING_DEBUG,
} setting_id_t;

typedef enum {
    SETTINGS_VIEW_MAIN,
    SETTINGS_VIEW_TIME,
    SETTINGS_VIEW_POWER,
    SETTINGS_VIEW_WIFI,
    SETTINGS_VIEW_NETWORKS,
    SETTINGS_VIEW_PROVISIONING,
    SETTINGS_VIEW_RELAY,
    SETTINGS_VIEW_DEBUG,
    SETTINGS_VIEW_LOG_VIEWER,
} settings_view_t;

typedef enum {
    TIME_ACTION_HOUR,
    TIME_ACTION_MINUTE,
    TIME_ACTION_SECOND,
    TIME_ACTION_FORMAT,
    TIME_ACTION_SYNC,
    TIME_ACTION_BACK,
} time_action_t;

typedef enum {
    POWER_ACTION_LIGHT_SLEEP,
    POWER_ACTION_WIFI_PS,
    POWER_ACTION_SCREEN_TIMEOUT,
    POWER_ACTION_AUTO_SLEEP,
    POWER_ACTION_SLEEP_NOW,
    POWER_ACTION_BACK,
} power_action_t;

typedef enum {
    WIFI_ACTION_TOGGLE,
    WIFI_ACTION_NETWORKS,
    WIFI_ACTION_SETUP,
    WIFI_ACTION_BACK,
} wifi_action_t;

typedef enum {
    RELAY_ACTION_PAIR,
    RELAY_ACTION_CLEAR,
    RELAY_ACTION_BACK,
} relay_action_t;

typedef enum {
    DEBUG_ACTION_TOGGLE,
    DEBUG_ACTION_SCREENCAST,
    DEBUG_ACTION_DEVICE_LOG,
    DEBUG_ACTION_NETWORK_LOG,
    DEBUG_ACTION_BACK,
} debug_action_t;

static const uint8_t BRIGHTNESS_LEVELS[] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
static const uint8_t VOLUME_LEVELS[] = { 0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };

static lv_obj_t *s_scr;
static lv_obj_t *s_items[SETTING_COUNT];
static lv_obj_t *s_titles[SETTING_COUNT];
static lv_obj_t *s_values[SETTING_COUNT];
static lv_obj_t *s_indicators[SETTING_COUNT];
static lv_obj_t *s_time_actions[TIME_ACTION_COUNT];
static lv_obj_t *s_time_action_titles[TIME_ACTION_COUNT];
static lv_obj_t *s_time_action_values[TIME_ACTION_COUNT];
static lv_obj_t *s_time_action_indicators[TIME_ACTION_COUNT];
static lv_obj_t *s_power_actions[POWER_ACTION_COUNT];
static lv_obj_t *s_power_action_titles[POWER_ACTION_COUNT];
static lv_obj_t *s_power_action_values[POWER_ACTION_COUNT];
static lv_obj_t *s_power_action_indicators[POWER_ACTION_COUNT];
static lv_obj_t *s_wifi_state_value;
static lv_obj_t *s_wifi_ssid_value;
static lv_obj_t *s_wifi_actions[WIFI_ACTION_COUNT];
static lv_obj_t *s_wifi_action_titles[WIFI_ACTION_COUNT];
static lv_obj_t *s_wifi_action_values[WIFI_ACTION_COUNT];
static lv_obj_t *s_wifi_action_indicators[WIFI_ACTION_COUNT];
static lv_obj_t *s_relay_status_value;
static lv_obj_t *s_relay_code_value;
static lv_obj_t *s_relay_actions[RELAY_ACTION_COUNT];
static lv_obj_t *s_relay_action_titles[RELAY_ACTION_COUNT];
static lv_obj_t *s_relay_action_values[RELAY_ACTION_COUNT];
static lv_obj_t *s_relay_action_indicators[RELAY_ACTION_COUNT];
static lv_obj_t *s_net_items[NET_ROW_MAX];
static lv_obj_t *s_net_titles[NET_ROW_MAX];
static lv_obj_t *s_net_values[NET_ROW_MAX];
static lv_obj_t *s_net_indicators[NET_ROW_MAX];
static lv_obj_t *s_net_hint;
static lv_obj_t *s_debug_actions[DEBUG_ACTION_COUNT];
static lv_obj_t *s_debug_action_titles[DEBUG_ACTION_COUNT];
static lv_obj_t *s_debug_action_values[DEBUG_ACTION_COUNT];
static lv_obj_t *s_debug_action_indicators[DEBUG_ACTION_COUNT];
static lv_obj_t *s_log_cont;
static lv_obj_t *s_log_label;
static lv_timer_t *s_refresh_timer;
static uint8_t s_selected;
static uint8_t s_time_selected;
static uint8_t s_power_selected;
static uint8_t s_wifi_selected;
static uint8_t s_net_selected;
static uint8_t s_relay_selected;
static uint8_t s_debug_selected;
static debug_log_type_t s_current_log_type = DEBUG_LOG_TYPE_DEVICE;
static uint8_t s_brightness_index;
static uint8_t s_volume_index;
static settings_view_t s_view;

static const char *wifi_state_text(void)
{
    switch (wifi_manager_get_state()) {
    case WIFI_MANAGER_CONNECTED: return "ONLINE";
    case WIFI_MANAGER_CONNECTING: return "WAIT";
    case WIFI_MANAGER_PROVISIONING: return "SETUP";
    case WIFI_MANAGER_FAILED: return "RETRY";
    case WIFI_MANAGER_UNCONFIGURED: return "SETUP";
    case WIFI_MANAGER_DISABLED: return "OFF";
    default: return "OFF";
    }
}

static const char *time_sync_state_text(void)
{
    switch (time_sync_get_state()) {
    case TIME_SYNC_SYNCING: return "SYNCING";
    case TIME_SYNC_SUCCESS: return "UPDATED";
    case TIME_SYNC_NO_WIFI: return "NO WI-FI";
    case TIME_SYNC_TIMEOUT: return "TIMEOUT";
    case TIME_SYNC_FAILED: return "ERROR";
    default: return "READY";
    }
}

static const char *relay_enrollment_text(const kiro_passport_enrollment_snapshot_t *enrollment)
{
    if (!kiro_passport_network_enrollment_supported()) return "NVS ENC OFF";
    switch (enrollment->state) {
    case KIRO_PASSPORT_ENROLLMENT_REQUESTING:
        if (wifi_manager_get_state() != WIFI_MANAGER_CONNECTED) return "WAIT WI-FI";
        return time_sync_get_state() == TIME_SYNC_SUCCESS ? "REQUESTING" : "SYNC TIME";
    case KIRO_PASSPORT_ENROLLMENT_WAITING_APPROVAL: return "PAIR IN BROWSER";
    case KIRO_PASSPORT_ENROLLMENT_ERROR:
        return enrollment->last_error == ESP_ERR_TIMEOUT ? "PAIR TIMEOUT" : "TLS/RELAY ERROR";
    default: return "NOT PAIRED";
    }
}

static void clear_main_objects(void)
{
    for (size_t i = 0; i < SETTING_COUNT; i++) {
        s_items[i] = NULL;
        s_titles[i] = NULL;
        s_values[i] = NULL;
        s_indicators[i] = NULL;
    }
}

static void clear_time_objects(void)
{
    for (size_t i = 0; i < TIME_ACTION_COUNT; i++) {
        s_time_actions[i] = NULL;
        s_time_action_titles[i] = NULL;
        s_time_action_values[i] = NULL;
        s_time_action_indicators[i] = NULL;
    }
}

static void clear_power_objects(void)
{
    for (size_t i = 0; i < POWER_ACTION_COUNT; i++) {
        s_power_actions[i] = NULL;
        s_power_action_titles[i] = NULL;
        s_power_action_values[i] = NULL;
        s_power_action_indicators[i] = NULL;
    }
}

static void clear_wifi_objects(void)
{
    s_wifi_state_value = NULL;
    s_wifi_ssid_value = NULL;
    for (size_t i = 0; i < WIFI_ACTION_COUNT; i++) {
        s_wifi_actions[i] = NULL;
        s_wifi_action_titles[i] = NULL;
        s_wifi_action_values[i] = NULL;
        s_wifi_action_indicators[i] = NULL;
    }
}

static void clear_net_objects(void)
{
    s_net_hint = NULL;
    for (size_t i = 0; i < NET_ROW_MAX; i++) {
        s_net_items[i] = NULL;
        s_net_titles[i] = NULL;
        s_net_values[i] = NULL;
        s_net_indicators[i] = NULL;
    }
}

static void clear_relay_objects(void)
{
    s_relay_status_value = NULL;
    s_relay_code_value = NULL;
    for (size_t i = 0; i < RELAY_ACTION_COUNT; i++) {
        s_relay_actions[i] = NULL;
        s_relay_action_titles[i] = NULL;
        s_relay_action_values[i] = NULL;
        s_relay_action_indicators[i] = NULL;
    }
}

static void clear_debug_objects(void)
{
    for (size_t i = 0; i < DEBUG_ACTION_COUNT; i++) {
        s_debug_actions[i] = NULL;
        s_debug_action_titles[i] = NULL;
        s_debug_action_values[i] = NULL;
        s_debug_action_indicators[i] = NULL;
    }
    s_log_cont = NULL;
    s_log_label = NULL;
}

static void settings_refresh(void)
{
    if (s_view != SETTINGS_VIEW_MAIN) return;

    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    ui_status_get_time(&hour, &minute, &second);
    kiro_passport_network_config_t relay_config;
    kiro_passport_network_get_config(&relay_config);
    kiro_passport_enrollment_snapshot_t enrollment;
    kiro_passport_network_get_enrollment(&enrollment);

    for (size_t i = 0; i < SETTING_COUNT; i++) {
        ui_system_set_item_state(s_items[i], s_titles[i], s_values[i],
                                 s_indicators[i], i == s_selected, true);
    }

    lv_label_set_text_fmt(s_values[SETTING_BRIGHTNESS], "%u%%",
                          (unsigned)BRIGHTNESS_LEVELS[s_brightness_index]);
    if (VOLUME_LEVELS[s_volume_index] == 0) {
        lv_label_set_text(s_values[SETTING_VOLUME], "0%");
    } else {
        lv_label_set_text_fmt(s_values[SETTING_VOLUME], "%u%%",
                              (unsigned)VOLUME_LEVELS[s_volume_index]);
    }
    if (ui_status_get_time_format() == UI_STATUS_TIME_HH_MM_SS) {
        lv_label_set_text_fmt(s_values[SETTING_TIME], "%02u:%02u:%02u",
                              (unsigned)hour, (unsigned)minute, (unsigned)second);
    } else {
        lv_label_set_text_fmt(s_values[SETTING_TIME], "%02u:%02u",
                              (unsigned)hour, (unsigned)minute);
    }
    lv_label_set_text(s_values[SETTING_POWER], "");
    lv_label_set_text(s_values[SETTING_WIFI], wifi_state_text());
    lv_label_set_text(s_values[SETTING_RELAY], relay_config.credential[0] ?
                      kiro_passport_network_state_name(kiro_passport_network_get_state()) :
                      relay_enrollment_text(&enrollment));
    lv_label_set_text(s_values[SETTING_DEBUG], debug_log_is_enabled() ? "ON" : "OFF");
}

static void power_details_refresh(void)
{
    if (s_view != SETTINGS_VIEW_POWER) return;

    for (size_t i = 0; i < POWER_ACTION_COUNT; i++) {
        ui_system_set_item_state(s_power_actions[i], s_power_action_titles[i],
                                 s_power_action_values[i], s_power_action_indicators[i],
                                 i == s_power_selected, true);
    }

    lv_label_set_text(s_power_action_values[POWER_ACTION_LIGHT_SLEEP],
                      power_manager_is_light_sleep_enabled() ? "ON" : "OFF");
    lv_label_set_text(s_power_action_values[POWER_ACTION_WIFI_PS],
                      wifi_manager_is_power_save_enabled() ? "ON" : "OFF");
    const app_settings_t *settings = app_settings_get();
    lv_label_set_text(s_power_action_values[POWER_ACTION_SCREEN_TIMEOUT],
                      app_settings_get_screen_timeout_text(settings->screen_timeout_index));
    lv_label_set_text(s_power_action_values[POWER_ACTION_AUTO_SLEEP],
                      app_settings_get_auto_sleep_timeout_text(settings->auto_sleep_timeout_index));
    lv_label_set_text(s_power_action_values[POWER_ACTION_SLEEP_NOW], "");
    lv_label_set_text(s_power_action_values[POWER_ACTION_BACK], "");
}

static void time_details_refresh(void)
{
    if (s_view != SETTINGS_VIEW_TIME) return;

    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    ui_status_get_time(&hour, &minute, &second);

    for (size_t i = 0; i < TIME_ACTION_COUNT; i++) {
        bool enabled = true;
        if (i == TIME_ACTION_SYNC) {
            enabled = wifi_manager_is_enabled();
        }
        ui_system_set_item_state(s_time_actions[i], s_time_action_titles[i],
                                 s_time_action_values[i], s_time_action_indicators[i],
                                 i == s_time_selected, enabled);
    }

    lv_label_set_text_fmt(s_time_action_values[TIME_ACTION_HOUR], "%02u", (unsigned)hour);
    lv_label_set_text_fmt(s_time_action_values[TIME_ACTION_MINUTE], "%02u", (unsigned)minute);
    lv_label_set_text_fmt(s_time_action_values[TIME_ACTION_SECOND], "%02u", (unsigned)second);
    lv_label_set_text(s_time_action_values[TIME_ACTION_FORMAT],
                      ui_status_get_time_format() == UI_STATUS_TIME_HH_MM_SS
                          ? "HH:MM:SS" : "HH:MM");
    lv_label_set_text(s_time_action_values[TIME_ACTION_SYNC],
                      wifi_manager_is_enabled() ? time_sync_state_text() : "OFF");
    lv_label_set_text(s_time_action_values[TIME_ACTION_BACK], "");
}

static void wifi_details_refresh(void)
{
    if (s_view != SETTINGS_VIEW_WIFI) return;

    bool enabled = wifi_manager_is_enabled();
    lv_label_set_text(s_wifi_state_value, wifi_state_text());
    char ssid[33];
    if (wifi_manager_get_connected_ssid(ssid, sizeof(ssid)) == ESP_OK && ssid[0]) {
        lv_label_set_text(s_wifi_ssid_value, ssid);
    } else {
        lv_label_set_text(s_wifi_ssid_value, "--");
    }

    for (size_t i = 0; i < WIFI_ACTION_COUNT; i++) {
        bool action_enabled = i == WIFI_ACTION_TOGGLE || i == WIFI_ACTION_BACK || enabled;
        ui_system_set_item_state(s_wifi_actions[i], s_wifi_action_titles[i],
                                 s_wifi_action_values[i], s_wifi_action_indicators[i],
                                 i == s_wifi_selected, action_enabled);
    }
    lv_label_set_text(s_wifi_action_values[WIFI_ACTION_TOGGLE], enabled ? "ON" : "OFF");
    lv_label_set_text_fmt(s_wifi_action_values[WIFI_ACTION_NETWORKS], "%u/%d",
                          (unsigned)wifi_nvs_count_profiles(), MAX_WIFI_PROFILES);
    lv_label_set_text(s_wifi_action_values[WIFI_ACTION_SETUP], "");
    lv_label_set_text(s_wifi_action_values[WIFI_ACTION_BACK], "");
}

static void networks_refresh(void)
{
    if (s_view != SETTINGS_VIEW_NETWORKS) return;

    wifi_profile_t profiles[MAX_WIFI_PROFILES];
    size_t count = wifi_nvs_get_all_profiles(profiles, MAX_WIFI_PROFILES);
    size_t rows = count + 1;
    if (s_net_selected >= rows) s_net_selected = 0;

    char connected[33];
    bool is_connected = wifi_manager_get_connected_ssid(connected, sizeof(connected)) == ESP_OK &&
                        connected[0];

    for (size_t i = 0; i < NET_ROW_MAX; i++) {
        const char *title;
        const char *value;
        bool enabled;
        if (i < count) {
            title = profiles[i].ssid;
            value = (is_connected && strcmp(connected, profiles[i].ssid) == 0)
                        ? "ONLINE" : "";
            enabled = wifi_manager_is_enabled();
        } else if (i == count) {
            title = "返回";
            value = "";
            enabled = true;
        } else {
            title = "";
            value = "";
            enabled = false;
        }
        lv_label_set_text(s_net_titles[i], title);
        lv_label_set_text(s_net_values[i], value);
        ui_system_set_item_state(s_net_items[i], s_net_titles[i], s_net_values[i],
                                 s_net_indicators[i], i == s_net_selected, enabled);
    }
    if (s_net_hint) {
        lv_label_set_text(s_net_hint, count ? "OK CONNECT  /  DOUBLE DELETE"
                                            : "NO SAVED NETWORKS");
    }
}

static void relay_details_refresh(void)
{
    if (s_view != SETTINGS_VIEW_RELAY) return;

    kiro_passport_network_config_t config;
    kiro_passport_network_get_config(&config);
    kiro_passport_enrollment_snapshot_t enrollment;
    kiro_passport_network_get_enrollment(&enrollment);
    bool enrolled = config.credential[0];
    bool pairing = enrollment.state == KIRO_PASSPORT_ENROLLMENT_REQUESTING ||
                   enrollment.state == KIRO_PASSPORT_ENROLLMENT_WAITING_APPROVAL;

    lv_label_set_text(s_relay_status_value, enrolled ?
                      kiro_passport_network_state_name(kiro_passport_network_get_state()) :
                      relay_enrollment_text(&enrollment));
    if (enrollment.state == KIRO_PASSPORT_ENROLLMENT_WAITING_APPROVAL) {
        lv_label_set_text_fmt(s_relay_code_value, "%s  %02u:%02u", enrollment.user_code,
                              (unsigned)(enrollment.expires_in_seconds / 60),
                              (unsigned)(enrollment.expires_in_seconds % 60));
    } else {
        lv_label_set_text(s_relay_code_value, "------");
    }

    bool pair_enabled = pairing || (!enrolled && kiro_passport_network_enrollment_supported());
    bool clear_enabled = enrolled && !pairing;
    for (size_t i = 0; i < RELAY_ACTION_COUNT; i++) {
        bool enabled = i == RELAY_ACTION_PAIR ? pair_enabled :
                       i == RELAY_ACTION_CLEAR ? clear_enabled : true;
        ui_system_set_item_state(s_relay_actions[i], s_relay_action_titles[i],
                                 s_relay_action_values[i], s_relay_action_indicators[i],
                                 i == s_relay_selected, enabled);
    }
    lv_label_set_text(s_relay_action_titles[RELAY_ACTION_PAIR], pairing ? "Cancel pairing" : "Start pairing");
    lv_label_set_text(s_relay_action_values[RELAY_ACTION_PAIR], pairing ? "" : "");
    lv_label_set_text(s_relay_action_titles[RELAY_ACTION_CLEAR], "Clear enrollment");
    lv_label_set_text(s_relay_action_values[RELAY_ACTION_CLEAR], "");
    lv_label_set_text(s_relay_action_titles[RELAY_ACTION_BACK], "Back");
    lv_label_set_text(s_relay_action_values[RELAY_ACTION_BACK], "");
}

static esp_err_t persist_settings(void)
{
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    ui_status_get_time(&hour, &minute, &second);

    app_settings_t settings = *app_settings_get();
    settings.brightness_index = s_brightness_index;
    settings.volume_index = s_volume_index;
    settings.hour = hour;
    settings.minute = minute;
    settings.second = second;
    settings.time_format = ui_status_get_time_format() == UI_STATUS_TIME_HH_MM_SS
                               ? APP_SETTINGS_TIME_HH_MM_SS : APP_SETTINGS_TIME_HH_MM;
    settings.wifi_enabled = wifi_manager_is_enabled();
    settings.light_sleep_enabled = power_manager_is_light_sleep_enabled();
    settings.wifi_power_save_enabled = wifi_manager_is_power_save_enabled();
    settings.debug_enabled = debug_log_is_enabled();
    settings.screencast_enabled = false;
    return app_settings_save(&settings);
}

static void debug_details_refresh(void);
static void log_viewer_refresh(void);

static void refresh_timer(lv_timer_t *timer)
{
    (void)timer;
    if (s_view == SETTINGS_VIEW_MAIN) {
        settings_refresh();
    } else if (s_view == SETTINGS_VIEW_TIME) {
        time_details_refresh();
    } else if (s_view == SETTINGS_VIEW_POWER) {
        power_details_refresh();
    } else if (s_view == SETTINGS_VIEW_WIFI) {
        wifi_details_refresh();
    } else if (s_view == SETTINGS_VIEW_NETWORKS) {
        networks_refresh();
    } else if (s_view == SETTINGS_VIEW_RELAY) {
        relay_details_refresh();
    } else if (s_view == SETTINGS_VIEW_DEBUG) {
        debug_details_refresh();
    } else if (s_view == SETTINGS_VIEW_LOG_VIEWER) {
        log_viewer_refresh();
    }
}

static void provisioning_build(void)
{
    s_scr = ui_system_screen_create();
    lv_obj_t *heading = ui_system_label(s_scr, "WI-FI", &lv_font_montserrat_20,
                                        UI_SYSTEM_TEXT);
    lv_obj_set_width(heading, 208);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(heading, 16, 42);
    ui_system_divider(s_scr, 16, 77, 208);

    const char *labels[] = { "AP", "密码", "地址" };
    const char *values[] = {
        wifi_manager_get_provisioning_ssid(),
        wifi_manager_get_provisioning_password(),
        "192.168.4.1",
    };
    for (size_t i = 0; i < 3; i++) {
        int y = 96 + (int)i * 48;
        lv_obj_t *label = ui_system_label(s_scr, labels[i], &ui_font_noto_sc_14,
                                          UI_SYSTEM_MUTED);
        lv_obj_set_pos(label, 24, y);
        lv_obj_t *value = ui_system_label(s_scr, values[i], &lv_font_montserrat_14,
                                          UI_SYSTEM_TEXT);
        lv_obj_set_pos(value, 76, y);
    }

    lv_obj_t *hint = ui_system_label(s_scr, "LONG OK: CANCEL", &lv_font_montserrat_14,
                                     UI_SYSTEM_MUTED);
    lv_obj_set_width(hint, 208);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(hint, 16, 266);
    lv_screen_load(s_scr);
}

static void power_details_build(void)
{
    s_scr = ui_system_screen_create();
    lv_obj_t *heading = ui_system_label(s_scr, "节能", &ui_font_noto_sc_20,
                                        UI_SYSTEM_TEXT);
    lv_obj_set_width(heading, 208);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(heading, 16, 42);
    ui_system_divider(s_scr, 16, 77, 208);

    static const char * const titles[POWER_ACTION_COUNT] = {
        "浅睡眠", "Wi-Fi 节能", "自动息屏", "自动休眠", "立刻休眠", "返回",
    };
    for (size_t i = 0; i < POWER_ACTION_COUNT; i++) {
        int y = 88 + (int)i * 30;
        s_power_actions[i] = ui_system_item_create(s_scr, 16, y, 208, 29);
        s_power_action_titles[i] = ui_system_label(s_power_actions[i], titles[i],
                                                   &ui_font_noto_sc_14, UI_SYSTEM_TEXT);
        lv_obj_set_pos(s_power_action_titles[i], 16, 6);
        s_power_action_values[i] = ui_system_label(s_power_actions[i], "",
                                                   &lv_font_montserrat_14, UI_SYSTEM_MUTED);
        lv_obj_set_width(s_power_action_values[i], 82);
        lv_obj_set_style_text_align(s_power_action_values[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_power_action_values[i], 82, 6);
        s_power_action_indicators[i] = ui_system_label(s_power_actions[i], ">",
                                                       &lv_font_montserrat_20, UI_SYSTEM_MUTED);
        lv_obj_set_pos(s_power_action_indicators[i], 180, 2);
    }
    power_details_refresh();
    lv_screen_load(s_scr);
}

static void time_details_build(void)
{
    s_scr = ui_system_screen_create();
    lv_obj_t *heading = ui_system_label(s_scr, "时间", &ui_font_noto_sc_20,
                                        UI_SYSTEM_TEXT);
    lv_obj_set_width(heading, 208);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(heading, 16, 42);
    ui_system_divider(s_scr, 16, 77, 208);

    static const char * const titles[TIME_ACTION_COUNT] = {
        "小时", "分钟", "秒钟", "时间格式", "同步时间", "返回",
    };
    for (size_t i = 0; i < TIME_ACTION_COUNT; i++) {
        int y = 88 + (int)i * 30;
        s_time_actions[i] = ui_system_item_create(s_scr, 16, y, 208, 29);
        s_time_action_titles[i] = ui_system_label(s_time_actions[i], titles[i],
                                                  &ui_font_noto_sc_14, UI_SYSTEM_TEXT);
        lv_obj_set_pos(s_time_action_titles[i], 16, 6);
        s_time_action_values[i] = ui_system_label(s_time_actions[i], "",
                                                  &lv_font_montserrat_14, UI_SYSTEM_MUTED);
        lv_obj_set_width(s_time_action_values[i], 82);
        lv_obj_set_style_text_align(s_time_action_values[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_time_action_values[i], 82, 6);
        s_time_action_indicators[i] = ui_system_label(s_time_actions[i], ">",
                                                      &lv_font_montserrat_20, UI_SYSTEM_MUTED);
        lv_obj_set_pos(s_time_action_indicators[i], 180, 2);
    }
    time_details_refresh();
    lv_screen_load(s_scr);
}

static void wifi_details_build(void)
{
    s_scr = ui_system_screen_create();
    lv_obj_t *heading = ui_system_label(s_scr, "WI-FI", &lv_font_montserrat_20,
                                        UI_SYSTEM_TEXT);
    lv_obj_set_width(heading, 208);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(heading, 16, 42);
    ui_system_divider(s_scr, 16, 77, 208);

    lv_obj_t *state_label = ui_system_label(s_scr, "状态", &ui_font_noto_sc_14,
                                            UI_SYSTEM_MUTED);
    lv_obj_set_pos(state_label, 16, 86);
    s_wifi_state_value = ui_system_label(s_scr, "", &lv_font_montserrat_14,
                                         UI_SYSTEM_TEXT);
    lv_obj_set_width(s_wifi_state_value, 130);
    lv_obj_set_style_text_align(s_wifi_state_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_wifi_state_value, 94, 86);

    lv_obj_t *ssid_label = ui_system_label(s_scr, "SSID", &lv_font_montserrat_14,
                                           UI_SYSTEM_MUTED);
    lv_obj_set_pos(ssid_label, 16, 111);
    s_wifi_ssid_value = ui_system_label(s_scr, "", &lv_font_montserrat_14,
                                        UI_SYSTEM_TEXT);
    lv_obj_set_width(s_wifi_ssid_value, 160);
    lv_label_set_long_mode(s_wifi_ssid_value, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_wifi_ssid_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_wifi_ssid_value, 64, 111);

    static const char * const titles[WIFI_ACTION_COUNT] = {
        "Wi-Fi 开关", "网络列表", "重新配网", "返回",
    };
    for (size_t i = 0; i < WIFI_ACTION_COUNT; i++) {
        int y = 139 + (int)i * 32;
        s_wifi_actions[i] = ui_system_item_create(s_scr, 16, y, 208, 29);
        s_wifi_action_titles[i] = ui_system_label(s_wifi_actions[i], titles[i],
                                                   &ui_font_noto_sc_14, UI_SYSTEM_TEXT);
        lv_obj_set_pos(s_wifi_action_titles[i], 16, 6);
        s_wifi_action_values[i] = ui_system_label(s_wifi_actions[i], "",
                                                   &lv_font_montserrat_14, UI_SYSTEM_MUTED);
        lv_obj_set_width(s_wifi_action_values[i], 82);
        lv_obj_set_style_text_align(s_wifi_action_values[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_wifi_action_values[i], 82, 6);
        s_wifi_action_indicators[i] = ui_system_label(s_wifi_actions[i], ">",
                                                       &lv_font_montserrat_20, UI_SYSTEM_MUTED);
        lv_obj_set_pos(s_wifi_action_indicators[i], 180, 2);
    }
    wifi_details_refresh();
    lv_screen_load(s_scr);
}

static void relay_details_build(void)
{
    s_scr = ui_system_screen_create();
    lv_obj_t *heading = ui_system_label(s_scr, "RELAY", &lv_font_montserrat_20,
                                        UI_SYSTEM_TEXT);
    lv_obj_set_width(heading, 208);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(heading, 16, 42);
    ui_system_divider(s_scr, 16, 77, 208);

    kiro_passport_network_config_t config;
    kiro_passport_network_get_config(&config);
    lv_obj_t *device_label = ui_system_label(s_scr, "DEVICE ID", &lv_font_montserrat_14,
                                             UI_SYSTEM_MUTED);
    lv_obj_set_pos(device_label, 16, 86);
    lv_obj_t *device_value = ui_system_label(s_scr, config.device_id, &lv_font_montserrat_14,
                                             UI_SYSTEM_TEXT);
    lv_obj_set_width(device_value, 208);
    lv_label_set_long_mode(device_value, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(device_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(device_value, 16, 86);

    lv_obj_t *status_label = ui_system_label(s_scr, "STATUS", &lv_font_montserrat_14,
                                             UI_SYSTEM_MUTED);
    lv_obj_set_pos(status_label, 16, 111);
    s_relay_status_value = ui_system_label(s_scr, "", &lv_font_montserrat_14, UI_SYSTEM_TEXT);
    lv_obj_set_width(s_relay_status_value, 132);
    lv_obj_set_style_text_align(s_relay_status_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_relay_status_value, 92, 111);

    lv_obj_t *code_label = ui_system_label(s_scr, "CODE", &lv_font_montserrat_14,
                                           UI_SYSTEM_MUTED);
    lv_obj_set_pos(code_label, 16, 136);
    s_relay_code_value = ui_system_label(s_scr, "", &lv_font_montserrat_14, UI_SYSTEM_TEXT);
    lv_obj_set_width(s_relay_code_value, 132);
    lv_obj_set_style_text_align(s_relay_code_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_relay_code_value, 92, 136);

    lv_obj_t *hint = ui_system_label(s_scr, "OPEN /admin/pair", &lv_font_montserrat_14,
                                     UI_SYSTEM_MUTED);
    lv_obj_set_width(hint, 208);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(hint, 16, 161);

    for (size_t i = 0; i < RELAY_ACTION_COUNT; i++) {
        int y = 188 + (int)i * 32;
        s_relay_actions[i] = ui_system_item_create(s_scr, 16, y, 208, 29);
        s_relay_action_titles[i] = ui_system_label(s_relay_actions[i], "",
                                                    &lv_font_montserrat_14, UI_SYSTEM_TEXT);
        lv_obj_set_pos(s_relay_action_titles[i], 16, 6);
        s_relay_action_values[i] = ui_system_label(s_relay_actions[i], "",
                                                    &lv_font_montserrat_14, UI_SYSTEM_MUTED);
        lv_obj_set_width(s_relay_action_values[i], 82);
        lv_obj_set_style_text_align(s_relay_action_values[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_relay_action_values[i], 82, 6);
        s_relay_action_indicators[i] = ui_system_label(s_relay_actions[i], ">",
                                                        &lv_font_montserrat_20, UI_SYSTEM_MUTED);
        lv_obj_set_pos(s_relay_action_indicators[i], 180, 2);
    }
    relay_details_refresh();
    lv_screen_load(s_scr);
}

static void networks_build(void)
{
    s_scr = ui_system_screen_create();
    lv_obj_t *heading = ui_system_label(s_scr, "网络", &ui_font_noto_sc_20,
                                        UI_SYSTEM_TEXT);
    lv_obj_set_width(heading, 208);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(heading, 16, 42);
    ui_system_divider(s_scr, 16, 77, 208);

    for (size_t i = 0; i < NET_ROW_MAX; i++) {
        int y = 92 + (int)i * 30;
        s_net_items[i] = ui_system_item_create(s_scr, 16, y, 208, 29);
        s_net_titles[i] = ui_system_label(s_net_items[i], "", &ui_font_noto_sc_14,
                                          UI_SYSTEM_TEXT);
        lv_obj_set_width(s_net_titles[i], 88);
        lv_label_set_long_mode(s_net_titles[i], LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s_net_titles[i], 16, 6);
        s_net_values[i] = ui_system_label(s_net_items[i], "", &lv_font_montserrat_14,
                                          UI_SYSTEM_MUTED);
        lv_obj_set_width(s_net_values[i], 62);
        lv_obj_set_style_text_align(s_net_values[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_net_values[i], 110, 6);
        s_net_indicators[i] = ui_system_label(s_net_items[i], ">",
                                              &lv_font_montserrat_20, UI_SYSTEM_MUTED);
        lv_obj_set_pos(s_net_indicators[i], 180, 2);
    }

    s_net_hint = ui_system_label(s_scr, "", &lv_font_montserrat_14, UI_SYSTEM_MUTED);
    lv_obj_set_width(s_net_hint, 208);
    lv_obj_set_style_text_align(s_net_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_net_hint, 16, 286);
    networks_refresh();
    lv_screen_load(s_scr);
}

static void debug_details_refresh(void)
{
    if (s_view != SETTINGS_VIEW_DEBUG) return;

    bool enabled = debug_log_is_enabled();
    for (size_t i = 0; i < DEBUG_ACTION_COUNT; i++) {
        ui_system_set_item_state(s_debug_actions[i], s_debug_action_titles[i],
                                 s_debug_action_values[i], s_debug_action_indicators[i],
                                 i == s_debug_selected, true);
    }
    lv_label_set_text(s_debug_action_values[DEBUG_ACTION_TOGGLE], enabled ? "ON" : "OFF");
    lv_label_set_text(s_debug_action_values[DEBUG_ACTION_SCREENCAST], "TEST");
    lv_label_set_text(s_debug_action_values[DEBUG_ACTION_DEVICE_LOG], "");
    lv_label_set_text(s_debug_action_values[DEBUG_ACTION_NETWORK_LOG], "");
    lv_label_set_text(s_debug_action_values[DEBUG_ACTION_BACK], "");
}

static void debug_details_build(void)
{
    s_scr = ui_system_screen_create();
    lv_obj_t *heading = ui_system_label(s_scr, "调试", &ui_font_noto_sc_20,
                                        UI_SYSTEM_TEXT);
    lv_obj_set_width(heading, 208);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(heading, 16, 32);
    ui_system_divider(s_scr, 16, 67, 208);

    static const char * const titles[DEBUG_ACTION_COUNT] = {
        "调试开关", "截屏测试", "设备日志", "网络日志", "返回",
    };
    for (size_t i = 0; i < DEBUG_ACTION_COUNT; i++) {
        int y = 78 + (int)i * 32;
        s_debug_actions[i] = ui_system_item_create(s_scr, 16, y, 208, 29);
        s_debug_action_titles[i] = ui_system_label(s_debug_actions[i], titles[i],
                                                   &ui_font_noto_sc_14, UI_SYSTEM_TEXT);
        lv_obj_set_pos(s_debug_action_titles[i], 16, 6);
        s_debug_action_values[i] = ui_system_label(s_debug_actions[i], "",
                                                   &lv_font_montserrat_14, UI_SYSTEM_MUTED);
        lv_obj_set_width(s_debug_action_values[i], 82);
        lv_obj_set_style_text_align(s_debug_action_values[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_debug_action_values[i], 82, 6);
        s_debug_action_indicators[i] = ui_system_label(s_debug_actions[i], ">",
                                                       &lv_font_montserrat_20, UI_SYSTEM_MUTED);
        lv_obj_set_pos(s_debug_action_indicators[i], 180, 2);
    }
    debug_details_refresh();
    lv_screen_load(s_scr);
}

static void log_viewer_refresh(void)
{
    if (s_view != SETTINGS_VIEW_LOG_VIEWER || !s_log_label) return;

    static char lines[DEBUG_LOG_MAX_LINES][DEBUG_LOG_LINE_MAX_LEN];
    size_t count = debug_log_get_lines(s_current_log_type, lines, DEBUG_LOG_MAX_LINES);

    if (count == 0) {
        lv_label_set_text(s_log_label, "(暂无日志记录)");
        return;
    }

    static char full_text[DEBUG_LOG_MAX_LINES * (DEBUG_LOG_LINE_MAX_LEN + 1) + 1];
    full_text[0] = '\0';
    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        size_t len = strlen(lines[i]);
        if (pos + len + 2 < sizeof(full_text)) {
            memcpy(full_text + pos, lines[i], len);
            pos += len;
            full_text[pos++] = '\n';
            full_text[pos] = '\0';
        }
    }
    lv_label_set_text(s_log_label, full_text);
}

static void log_viewer_build(void)
{
    s_scr = ui_system_screen_create();
    const char *title = (s_current_log_type == DEBUG_LOG_TYPE_NETWORK) ? "网络日志" : "设备日志";
    lv_obj_t *heading = ui_system_label(s_scr, title, &ui_font_noto_sc_14,
                                        UI_SYSTEM_TEXT);
    lv_obj_set_width(heading, 208);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(heading, 16, 32);
    ui_system_divider(s_scr, 16, 62, 208);

    s_log_cont = lv_obj_create(s_scr);
    lv_obj_set_pos(s_log_cont, 14, 68);
    lv_obj_set_size(s_log_cont, 212, 216);
    lv_obj_set_style_bg_color(s_log_cont, lv_color_hex(UI_SYSTEM_SURFACE), 0);
    lv_obj_set_style_border_color(s_log_cont, lv_color_hex(UI_SYSTEM_BORDER), 0);
    lv_obj_set_style_border_width(s_log_cont, 1, 0);
    lv_obj_set_style_radius(s_log_cont, 4, 0);
    lv_obj_set_style_pad_all(s_log_cont, 6, 0);
    lv_obj_add_flag(s_log_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_log_cont, LV_SCROLLBAR_MODE_AUTO);

    s_log_label = lv_label_create(s_log_cont);
    lv_obj_set_width(s_log_label, 196);
    lv_label_set_long_mode(s_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_log_label, &ui_font_noto_sc_14, 0);
    lv_obj_set_style_text_color(s_log_label, lv_color_hex(UI_SYSTEM_TEXT), 0);

    lv_obj_t *hint = ui_system_label(s_scr, "UP/DOWN 滚动  OK 返回", &ui_font_noto_sc_14,
                                     UI_SYSTEM_MUTED);
    lv_obj_set_width(hint, 208);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(hint, 16, 292);

    log_viewer_refresh();
    lv_obj_scroll_to_y(s_log_cont, LV_COORD_MAX, LV_ANIM_OFF);
    lv_screen_load(s_scr);
}

static void settings_build(void)
{
    if (s_view == SETTINGS_VIEW_TIME) {
        time_details_build();
        return;
    }
    if (s_view == SETTINGS_VIEW_POWER) {
        power_details_build();
        return;
    }
    if (s_view == SETTINGS_VIEW_WIFI) {
        wifi_details_build();
        return;
    }
    if (s_view == SETTINGS_VIEW_NETWORKS) {
        networks_build();
        return;
    }
    if (s_view == SETTINGS_VIEW_PROVISIONING) {
        provisioning_build();
        return;
    }
    if (s_view == SETTINGS_VIEW_RELAY) {
        relay_details_build();
        return;
    }
    if (s_view == SETTINGS_VIEW_DEBUG) {
        debug_details_build();
        return;
    }
    if (s_view == SETTINGS_VIEW_LOG_VIEWER) {
        log_viewer_build();
        return;
    }

    s_scr = ui_system_screen_create();
    lv_obj_t *back = ui_system_label(s_scr, "<", &lv_font_montserrat_20, UI_SYSTEM_TEXT);
    lv_obj_set_pos(back, 18, 36);
    lv_obj_t *heading = ui_system_label(s_scr, "设置", &ui_font_noto_sc_20, UI_SYSTEM_TEXT);
    lv_obj_set_width(heading, 208);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(heading, 16, 36);
    ui_system_divider(s_scr, 16, 68, 208);

    static const char * const titles[SETTING_COUNT] = {
        "亮度", "音量", "时间", "节能", "网络", "Relay", "调试",
    };
    for (size_t i = 0; i < SETTING_COUNT; i++) {
        int y = 74 + (int)i * 33;
        s_items[i] = ui_system_item_create(s_scr, 16, y, 208, 29);
        s_titles[i] = ui_system_label(s_items[i], titles[i], &ui_font_noto_sc_14,
                                      UI_SYSTEM_TEXT);
        lv_obj_set_pos(s_titles[i], 16, 6);
        s_values[i] = ui_system_label(s_items[i], "", &lv_font_montserrat_14,
                                      UI_SYSTEM_MUTED);
        lv_obj_set_width(s_values[i], 82);
        lv_obj_set_style_text_align(s_values[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_values[i], 82, 6);
        s_indicators[i] = ui_system_label(s_items[i], ">", &lv_font_montserrat_20,
                                          UI_SYSTEM_MUTED);
        lv_obj_set_pos(s_indicators[i], 180, 2);
    }
    settings_refresh();
    lv_screen_load(s_scr);
}

static void show_view(settings_view_t view)
{
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    clear_main_objects();
    clear_time_objects();
    clear_power_objects();
    clear_wifi_objects();
    clear_net_objects();
    clear_relay_objects();
    clear_debug_objects();
    s_view = view;
    settings_build();
}

void demo_settings_enter(void)
{
    s_brightness_index = app_settings_get()->brightness_index;
    if (s_brightness_index >= sizeof(BRIGHTNESS_LEVELS) / sizeof(BRIGHTNESS_LEVELS[0])) {
        s_brightness_index = (sizeof(BRIGHTNESS_LEVELS) / sizeof(BRIGHTNESS_LEVELS[0])) - 1;
    }
    bsp_display_backlight(BRIGHTNESS_LEVELS[s_brightness_index]);
    s_volume_index = app_settings_get()->volume_index;
    if (s_volume_index >= sizeof(VOLUME_LEVELS) / sizeof(VOLUME_LEVELS[0])) {
        s_volume_index = 8;
    }
    s_selected = 0;
    s_time_selected = 0;
    s_power_selected = 0;
    s_wifi_selected = 0;
    s_net_selected = 0;
    s_relay_selected = 0;
    s_debug_selected = 0;
    s_view = SETTINGS_VIEW_MAIN;
    settings_build();
    s_refresh_timer = lv_timer_create(refresh_timer, 500, NULL);
    ui_status_set_visible(true);
}

void demo_settings_exit(void)
{
    if (s_refresh_timer) {
        lv_timer_delete(s_refresh_timer);
        s_refresh_timer = NULL;
    }
    wifi_manager_stop_provisioning();
    ui_status_set_visible(false);
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    clear_main_objects();
    clear_time_objects();
    clear_power_objects();
    clear_wifi_objects();
    clear_net_objects();
    clear_relay_objects();
    clear_debug_objects();
    s_view = SETTINGS_VIEW_MAIN;
}

static void power_settings_key(bsp_btn_t btn)
{
    if (btn == BSP_BTN_UP) {
        s_power_selected = (s_power_selected + POWER_ACTION_COUNT - 1) % POWER_ACTION_COUNT;
        power_details_refresh();
        return;
    }
    if (btn == BSP_BTN_DOWN) {
        s_power_selected = (s_power_selected + 1) % POWER_ACTION_COUNT;
        power_details_refresh();
        return;
    }
    if (btn != BSP_BTN_OK) return;

    app_settings_t settings = *app_settings_get();
    switch ((power_action_t)s_power_selected) {
    case POWER_ACTION_LIGHT_SLEEP: {
        bool previous = power_manager_is_light_sleep_enabled();
        if (power_manager_set_light_sleep_enabled(!previous) == ESP_OK) {
            settings.light_sleep_enabled = !previous;
            app_settings_save(&settings);
        }
        break;
    }
    case POWER_ACTION_WIFI_PS: {
        bool previous = wifi_manager_is_power_save_enabled();
        if (wifi_manager_set_power_save(!previous) == ESP_OK) {
            settings.wifi_power_save_enabled = !previous;
            app_settings_save(&settings);
        }
        break;
    }
    case POWER_ACTION_SCREEN_TIMEOUT:
        settings.screen_timeout_index = (settings.screen_timeout_index + 1) % APP_SETTINGS_SCREEN_TIMEOUT_COUNT;
        app_settings_save(&settings);
        break;
    case POWER_ACTION_AUTO_SLEEP:
        settings.auto_sleep_timeout_index = (settings.auto_sleep_timeout_index + 1) % APP_SETTINGS_AUTO_SLEEP_COUNT;
        app_settings_save(&settings);
        break;
    case POWER_ACTION_SLEEP_NOW:
        power_manager_enter_deep_sleep();
        return;
    case POWER_ACTION_BACK:
        s_selected = SETTING_POWER;
        show_view(SETTINGS_VIEW_MAIN);
        return;
    }
    power_details_refresh();
}

static void main_settings_key(bsp_btn_t btn)
{
    if (btn == BSP_BTN_UP) {
        s_selected = (s_selected + SETTING_COUNT - 1) % SETTING_COUNT;
        settings_refresh();
        return;
    }
    if (btn == BSP_BTN_DOWN) {
        s_selected = (s_selected + 1) % SETTING_COUNT;
        settings_refresh();
        return;
    }
    if (btn != BSP_BTN_OK) return;

    switch ((setting_id_t)s_selected) {
    case SETTING_BRIGHTNESS:
        s_brightness_index = (s_brightness_index + 1) %
                             (sizeof(BRIGHTNESS_LEVELS) / sizeof(BRIGHTNESS_LEVELS[0]));
        bsp_display_backlight(BRIGHTNESS_LEVELS[s_brightness_index]);
        persist_settings();
        break;
    case SETTING_VOLUME:
        s_volume_index = (s_volume_index + 1) %
                         (sizeof(VOLUME_LEVELS) / sizeof(VOLUME_LEVELS[0]));
        bsp_audio_set_volume(VOLUME_LEVELS[s_volume_index]);
        persist_settings();
        game_audio_play(GAME_SFX_MOVE);
        break;
    case SETTING_TIME:
        s_time_selected = 0;
        show_view(SETTINGS_VIEW_TIME);
        return;
    case SETTING_POWER:
        s_power_selected = 0;
        show_view(SETTINGS_VIEW_POWER);
        return;
    case SETTING_WIFI:
        if (wifi_manager_get_state() == WIFI_MANAGER_PROVISIONING) {
            show_view(SETTINGS_VIEW_PROVISIONING);
        } else {
            s_wifi_selected = 0;
            show_view(SETTINGS_VIEW_WIFI);
        }
        return;
    case SETTING_RELAY:
        s_relay_selected = 0;
        show_view(SETTINGS_VIEW_RELAY);
        return;
    case SETTING_DEBUG:
        s_debug_selected = 0;
        show_view(SETTINGS_VIEW_DEBUG);
        return;
    }
    settings_refresh();
}

static void time_settings_key(bsp_btn_t btn)
{
    if (btn == BSP_BTN_UP) {
        s_time_selected = (s_time_selected + TIME_ACTION_COUNT - 1) % TIME_ACTION_COUNT;
        time_details_refresh();
        return;
    }
    if (btn == BSP_BTN_DOWN) {
        s_time_selected = (s_time_selected + 1) % TIME_ACTION_COUNT;
        time_details_refresh();
        return;
    }
    if (btn != BSP_BTN_OK) return;

    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    ui_status_get_time(&hour, &minute, &second);

    switch ((time_action_t)s_time_selected) {
    case TIME_ACTION_HOUR:
        ui_status_set_time((hour + 1) % 24, minute, second);
        persist_settings();
        break;
    case TIME_ACTION_MINUTE:
        ui_status_set_time(hour, (minute + 1) % 60, second);
        persist_settings();
        break;
    case TIME_ACTION_SECOND:
        ui_status_set_time(hour, minute, (second + 1) % 60);
        persist_settings();
        break;
    case TIME_ACTION_FORMAT:
        ui_status_set_time_format(ui_status_get_time_format() == UI_STATUS_TIME_HH_MM
                                      ? UI_STATUS_TIME_HH_MM_SS : UI_STATUS_TIME_HH_MM);
        persist_settings();
        break;
    case TIME_ACTION_SYNC:
        if (wifi_manager_is_enabled()) {
            time_sync_request();
        }
        break;
    case TIME_ACTION_BACK:
        s_selected = SETTING_TIME;
        show_view(SETTINGS_VIEW_MAIN);
        return;
    }
    time_details_refresh();
}

static void wifi_settings_key(bsp_btn_t btn)
{
    if (btn == BSP_BTN_UP) {
        s_wifi_selected = (s_wifi_selected + WIFI_ACTION_COUNT - 1) % WIFI_ACTION_COUNT;
        wifi_details_refresh();
        return;
    }
    if (btn == BSP_BTN_DOWN) {
        s_wifi_selected = (s_wifi_selected + 1) % WIFI_ACTION_COUNT;
        wifi_details_refresh();
        return;
    }
    if (btn != BSP_BTN_OK) return;

    switch ((wifi_action_t)s_wifi_selected) {
    case WIFI_ACTION_TOGGLE:
        if (wifi_manager_set_enabled(!wifi_manager_is_enabled()) == ESP_OK) {
            wifi_manager_set_power_save(wifi_manager_is_power_save_enabled());
            persist_settings();
        }
        wifi_details_refresh();
        break;
    case WIFI_ACTION_NETWORKS:
        s_net_selected = 0;
        show_view(SETTINGS_VIEW_NETWORKS);
        break;
    case WIFI_ACTION_SETUP:
        if (wifi_manager_is_enabled() && wifi_manager_start_provisioning() == ESP_OK) {
            show_view(SETTINGS_VIEW_PROVISIONING);
        }
        break;
    case WIFI_ACTION_BACK:
        s_selected = SETTING_WIFI;
        show_view(SETTINGS_VIEW_MAIN);
        break;
    }
}

static void networks_settings_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    wifi_profile_t profiles[MAX_WIFI_PROFILES];
    size_t count = wifi_nvs_get_all_profiles(profiles, MAX_WIFI_PROFILES);
    size_t rows = count + 1;

    if (ev == BSP_BTN_DOUBLE) {
        if (btn == BSP_BTN_OK && s_net_selected < count) {
            if (wifi_manager_remove_profile(profiles[s_net_selected].ssid) == ESP_OK) {
                s_net_selected = 0;
                show_view(SETTINGS_VIEW_NETWORKS);
            }
        }
        return;
    }

    if (btn == BSP_BTN_UP) {
        s_net_selected = (s_net_selected + rows - 1) % rows;
        networks_refresh();
        return;
    }
    if (btn == BSP_BTN_DOWN) {
        s_net_selected = (s_net_selected + 1) % rows;
        networks_refresh();
        return;
    }
    if (btn != BSP_BTN_OK) return;

    if (s_net_selected < count) {
        wifi_manager_set_active_profile(profiles[s_net_selected].ssid);
    } else {
        s_wifi_selected = WIFI_ACTION_NETWORKS;
        show_view(SETTINGS_VIEW_WIFI);
    }
    networks_refresh();
}

static void relay_settings_key(bsp_btn_t btn)
{
    if (btn == BSP_BTN_UP) {
        s_relay_selected = (s_relay_selected + RELAY_ACTION_COUNT - 1) % RELAY_ACTION_COUNT;
        relay_details_refresh();
        return;
    }
    if (btn == BSP_BTN_DOWN) {
        s_relay_selected = (s_relay_selected + 1) % RELAY_ACTION_COUNT;
        relay_details_refresh();
        return;
    }
    if (btn != BSP_BTN_OK) return;

    kiro_passport_enrollment_snapshot_t enrollment;
    kiro_passport_network_get_enrollment(&enrollment);
    switch ((relay_action_t)s_relay_selected) {
    case RELAY_ACTION_PAIR:
        if (enrollment.state == KIRO_PASSPORT_ENROLLMENT_REQUESTING ||
            enrollment.state == KIRO_PASSPORT_ENROLLMENT_WAITING_APPROVAL) {
            kiro_passport_network_cancel_enrollment();
        } else {
            kiro_passport_network_start_enrollment();
        }
        relay_details_refresh();
        break;
    case RELAY_ACTION_CLEAR:
        kiro_passport_network_clear_configuration();
        relay_details_refresh();
        break;
    case RELAY_ACTION_BACK:
        s_selected = SETTING_RELAY;
        show_view(SETTINGS_VIEW_MAIN);
        break;
    }
}

static void debug_settings_key(bsp_btn_t btn)
{
    if (btn == BSP_BTN_UP) {
        s_debug_selected = (s_debug_selected + DEBUG_ACTION_COUNT - 1) % DEBUG_ACTION_COUNT;
        debug_details_refresh();
        return;
    }
    if (btn == BSP_BTN_DOWN) {
        s_debug_selected = (s_debug_selected + 1) % DEBUG_ACTION_COUNT;
        debug_details_refresh();
        return;
    }
    if (btn != BSP_BTN_OK) return;

    switch ((debug_action_t)s_debug_selected) {
    case DEBUG_ACTION_TOGGLE:
        debug_log_set_enabled(!debug_log_is_enabled());
        persist_settings();
        debug_details_refresh();
        break;
    case DEBUG_ACTION_SCREENCAST:
        screencast_request_capture();
        lv_label_set_text(s_debug_action_values[DEBUG_ACTION_SCREENCAST], "SENT");
        break;
    case DEBUG_ACTION_DEVICE_LOG:
        s_current_log_type = DEBUG_LOG_TYPE_DEVICE;
        show_view(SETTINGS_VIEW_LOG_VIEWER);
        break;
    case DEBUG_ACTION_NETWORK_LOG:
        s_current_log_type = DEBUG_LOG_TYPE_NETWORK;
        show_view(SETTINGS_VIEW_LOG_VIEWER);
        break;
    case DEBUG_ACTION_BACK:
        s_selected = SETTING_DEBUG;
        show_view(SETTINGS_VIEW_MAIN);
        break;
    }
}

static void log_viewer_key(bsp_btn_t btn)
{
    if (btn == BSP_BTN_UP) {
        if (s_log_cont) {
            lv_obj_scroll_by_bounded(s_log_cont, 0, 48, LV_ANIM_OFF);
        }
        return;
    }
    if (btn == BSP_BTN_DOWN) {
        if (s_log_cont) {
            lv_obj_scroll_by_bounded(s_log_cont, 0, -48, LV_ANIM_OFF);
        }
        return;
    }
    if (btn == BSP_BTN_OK) {
        show_view(SETTINGS_VIEW_DEBUG);
        return;
    }
}

void demo_settings_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (s_view == SETTINGS_VIEW_PROVISIONING) return;
    if (s_view == SETTINGS_VIEW_NETWORKS) {
        if (ev == BSP_BTN_CLICK || ev == BSP_BTN_DOUBLE) {
            networks_settings_key(btn, ev);
        }
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (s_view == SETTINGS_VIEW_TIME) {
        time_settings_key(btn);
    } else if (s_view == SETTINGS_VIEW_POWER) {
        power_settings_key(btn);
    } else if (s_view == SETTINGS_VIEW_WIFI) {
        wifi_settings_key(btn);
    } else if (s_view == SETTINGS_VIEW_RELAY) {
        relay_settings_key(btn);
    } else if (s_view == SETTINGS_VIEW_DEBUG) {
        debug_settings_key(btn);
    } else if (s_view == SETTINGS_VIEW_LOG_VIEWER) {
        log_viewer_key(btn);
    } else {
        main_settings_key(btn);
    }
}
