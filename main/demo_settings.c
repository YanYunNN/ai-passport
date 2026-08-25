#include "app_settings.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "kiro_passport_network.h"
#include "power_manager.h"
#include "time_sync.h"
#include "ui_font_noto_sc_14.h"
#include "ui_font_noto_sc_20.h"
#include "ui_status.h"
#include "ui_system.h"
#include "wifi_manager.h"
#include "lvgl.h"

#define SETTING_COUNT 5
#define TIME_ACTION_COUNT 5
#define WIFI_ACTION_COUNT 5
#define RELAY_ACTION_COUNT 3

typedef enum {
    SETTING_BRIGHTNESS,
    SETTING_TIME,
    SETTING_LIGHT_SLEEP,
    SETTING_WIFI,
    SETTING_RELAY,
} setting_id_t;

typedef enum {
    SETTINGS_VIEW_MAIN,
    SETTINGS_VIEW_TIME,
    SETTINGS_VIEW_WIFI,
    SETTINGS_VIEW_PROVISIONING,
    SETTINGS_VIEW_RELAY,
} settings_view_t;

typedef enum {
    TIME_ACTION_HOUR,
    TIME_ACTION_MINUTE,
    TIME_ACTION_SECOND,
    TIME_ACTION_FORMAT,
    TIME_ACTION_BACK,
} time_action_t;

typedef enum {
    WIFI_ACTION_TOGGLE,
    WIFI_ACTION_POWER_SAVE,
    WIFI_ACTION_SETUP,
    WIFI_ACTION_SYNC_TIME,
    WIFI_ACTION_BACK,
} wifi_action_t;

typedef enum {
    RELAY_ACTION_PAIR,
    RELAY_ACTION_CLEAR,
    RELAY_ACTION_BACK,
} relay_action_t;

static const uint8_t BRIGHTNESS_LEVELS[] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };

static lv_obj_t *s_scr;
static lv_obj_t *s_items[SETTING_COUNT];
static lv_obj_t *s_titles[SETTING_COUNT];
static lv_obj_t *s_values[SETTING_COUNT];
static lv_obj_t *s_indicators[SETTING_COUNT];
static lv_obj_t *s_time_actions[TIME_ACTION_COUNT];
static lv_obj_t *s_time_action_titles[TIME_ACTION_COUNT];
static lv_obj_t *s_time_action_values[TIME_ACTION_COUNT];
static lv_obj_t *s_time_action_indicators[TIME_ACTION_COUNT];
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
static lv_timer_t *s_refresh_timer;
static uint8_t s_selected;
static uint8_t s_time_selected;
static uint8_t s_wifi_selected;
static uint8_t s_relay_selected;
static uint8_t s_brightness_index;
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
    if (ui_status_get_time_format() == UI_STATUS_TIME_HH_MM_SS) {
        lv_label_set_text_fmt(s_values[SETTING_TIME], "%02u:%02u:%02u",
                              (unsigned)hour, (unsigned)minute, (unsigned)second);
    } else {
        lv_label_set_text_fmt(s_values[SETTING_TIME], "%02u:%02u",
                              (unsigned)hour, (unsigned)minute);
    }
    lv_label_set_text(s_values[SETTING_LIGHT_SLEEP],
                      power_manager_is_light_sleep_enabled() ? "ON" : "OFF");
    lv_label_set_text(s_values[SETTING_WIFI], wifi_state_text());
    lv_label_set_text(s_values[SETTING_RELAY], relay_config.credential[0] ?
                      kiro_passport_network_state_name(kiro_passport_network_get_state()) :
                      relay_enrollment_text(&enrollment));
}

static void time_details_refresh(void)
{
    if (s_view != SETTINGS_VIEW_TIME) return;

    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    ui_status_get_time(&hour, &minute, &second);

    for (size_t i = 0; i < TIME_ACTION_COUNT; i++) {
        ui_system_set_item_state(s_time_actions[i], s_time_action_titles[i],
                                 s_time_action_values[i], s_time_action_indicators[i],
                                 i == s_time_selected, true);
    }

    lv_label_set_text_fmt(s_time_action_values[TIME_ACTION_HOUR], "%02u", (unsigned)hour);
    lv_label_set_text_fmt(s_time_action_values[TIME_ACTION_MINUTE], "%02u", (unsigned)minute);
    lv_label_set_text_fmt(s_time_action_values[TIME_ACTION_SECOND], "%02u", (unsigned)second);
    lv_label_set_text(s_time_action_values[TIME_ACTION_FORMAT],
                      ui_status_get_time_format() == UI_STATUS_TIME_HH_MM_SS
                          ? "HH:MM:SS" : "HH:MM");
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
    lv_label_set_text(s_wifi_action_values[WIFI_ACTION_POWER_SAVE],
                      wifi_manager_is_power_save_enabled() ? "ON" : "OFF");
    lv_label_set_text(s_wifi_action_values[WIFI_ACTION_SETUP], "");
    lv_label_set_text(s_wifi_action_values[WIFI_ACTION_SYNC_TIME],
                      enabled ? time_sync_state_text() : "OFF");
    lv_label_set_text(s_wifi_action_values[WIFI_ACTION_BACK], "");
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

    app_settings_t settings = {
        .brightness_index = s_brightness_index,
        .hour = hour,
        .minute = minute,
        .second = second,
        .time_format = ui_status_get_time_format() == UI_STATUS_TIME_HH_MM_SS
                           ? APP_SETTINGS_TIME_HH_MM_SS : APP_SETTINGS_TIME_HH_MM,
        .wifi_enabled = wifi_manager_is_enabled(),
        .light_sleep_enabled = power_manager_is_light_sleep_enabled(),
        .wifi_power_save_enabled = wifi_manager_is_power_save_enabled(),
    };
    return app_settings_save(&settings);
}

static void refresh_timer(lv_timer_t *timer)
{
    (void)timer;
    if (s_view == SETTINGS_VIEW_MAIN) {
        settings_refresh();
    } else if (s_view == SETTINGS_VIEW_TIME) {
        time_details_refresh();
    } else if (s_view == SETTINGS_VIEW_WIFI) {
        wifi_details_refresh();
    } else if (s_view == SETTINGS_VIEW_RELAY) {
        relay_details_refresh();
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
        "小时", "分钟", "秒钟", "时间格式", "返回",
    };
    for (size_t i = 0; i < TIME_ACTION_COUNT; i++) {
        int y = 92 + (int)i * 32;
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
        "Wi-Fi 开关", "网络节能", "重新配网", "同步时间", "返回",
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

static void settings_build(void)
{
    if (s_view == SETTINGS_VIEW_TIME) {
        time_details_build();
        return;
    }
    if (s_view == SETTINGS_VIEW_WIFI) {
        wifi_details_build();
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

    s_scr = ui_system_screen_create();
    lv_obj_t *back = ui_system_label(s_scr, "<", &lv_font_montserrat_20, UI_SYSTEM_TEXT);
    lv_obj_set_pos(back, 18, 42);
    lv_obj_t *heading = ui_system_label(s_scr, "设置", &ui_font_noto_sc_20, UI_SYSTEM_TEXT);
    lv_obj_set_width(heading, 208);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(heading, 16, 42);
    ui_system_divider(s_scr, 16, 77, 208);

    static const char * const titles[SETTING_COUNT] = {
        "亮度", "时间", "浅睡眠", "网络", "Relay",
    };
    for (size_t i = 0; i < SETTING_COUNT; i++) {
        int y = 92 + (int)i * 32;
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
    clear_wifi_objects();
    clear_relay_objects();
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
    s_selected = 0;
    s_time_selected = 0;
    s_wifi_selected = 0;
    s_relay_selected = 0;
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
    clear_wifi_objects();
    clear_relay_objects();
    s_view = SETTINGS_VIEW_MAIN;
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
    case SETTING_TIME:
        s_time_selected = 0;
        show_view(SETTINGS_VIEW_TIME);
        return;
    case SETTING_LIGHT_SLEEP: {
        bool previous = power_manager_is_light_sleep_enabled();
        if (power_manager_set_light_sleep_enabled(!previous) == ESP_OK &&
            persist_settings() != ESP_OK) {
            power_manager_set_light_sleep_enabled(previous);
        }
        break;
    }
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
    case WIFI_ACTION_POWER_SAVE: {
        bool previous = wifi_manager_is_power_save_enabled();
        if (wifi_manager_set_power_save(!previous) == ESP_OK && persist_settings() != ESP_OK) {
            wifi_manager_set_power_save(previous);
        }
        wifi_details_refresh();
        break;
    }
    case WIFI_ACTION_SETUP:
        if (wifi_manager_is_enabled() && wifi_manager_start_provisioning() == ESP_OK) {
            show_view(SETTINGS_VIEW_PROVISIONING);
        }
        break;
    case WIFI_ACTION_SYNC_TIME:
        if (wifi_manager_is_enabled()) time_sync_request();
        wifi_details_refresh();
        break;
    case WIFI_ACTION_BACK:
        s_selected = SETTING_WIFI;
        show_view(SETTINGS_VIEW_MAIN);
        break;
    }
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

void demo_settings_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK || s_view == SETTINGS_VIEW_PROVISIONING) return;
    if (s_view == SETTINGS_VIEW_TIME) {
        time_settings_key(btn);
    } else if (s_view == SETTINGS_VIEW_WIFI) {
        wifi_settings_key(btn);
    } else if (s_view == SETTINGS_VIEW_RELAY) {
        relay_settings_key(btn);
    } else {
        main_settings_key(btn);
    }
}
