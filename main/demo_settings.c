#include "app_settings.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "time_sync.h"
#include "ui_font_noto_sc_14.h"
#include "ui_font_noto_sc_20.h"
#include "ui_status.h"
#include "ui_system.h"
#include "wifi_manager.h"
#include "lvgl.h"

#define SETTING_COUNT 7
#define WIFI_ACTION_COUNT 4

typedef enum {
    SETTING_BRIGHTNESS,
    SETTING_HOUR,
    SETTING_MINUTE,
    SETTING_SECOND,
    SETTING_TIME_FORMAT,
    SETTING_WIFI,
    SETTING_RESET,
} setting_id_t;

typedef enum {
    SETTINGS_VIEW_MAIN,
    SETTINGS_VIEW_WIFI,
    SETTINGS_VIEW_PROVISIONING,
} settings_view_t;

typedef enum {
    WIFI_ACTION_TOGGLE,
    WIFI_ACTION_SETUP,
    WIFI_ACTION_SYNC_TIME,
    WIFI_ACTION_BACK,
} wifi_action_t;

static const uint8_t BRIGHTNESS_LEVELS[] = { 30, 60, 100 };

static lv_obj_t *s_scr;
static lv_obj_t *s_items[SETTING_COUNT];
static lv_obj_t *s_titles[SETTING_COUNT];
static lv_obj_t *s_values[SETTING_COUNT];
static lv_obj_t *s_indicators[SETTING_COUNT];
static lv_obj_t *s_wifi_state_value;
static lv_obj_t *s_wifi_ssid_value;
static lv_obj_t *s_wifi_actions[WIFI_ACTION_COUNT];
static lv_obj_t *s_wifi_action_titles[WIFI_ACTION_COUNT];
static lv_obj_t *s_wifi_action_values[WIFI_ACTION_COUNT];
static lv_obj_t *s_wifi_action_indicators[WIFI_ACTION_COUNT];
static lv_timer_t *s_refresh_timer;
static uint8_t s_selected;
static uint8_t s_wifi_selected;
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

static void clear_main_objects(void)
{
    for (size_t i = 0; i < SETTING_COUNT; i++) {
        s_items[i] = NULL;
        s_titles[i] = NULL;
        s_values[i] = NULL;
        s_indicators[i] = NULL;
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

static void settings_refresh(void)
{
    if (s_view != SETTINGS_VIEW_MAIN) return;

    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    ui_status_get_time(&hour, &minute, &second);

    for (size_t i = 0; i < SETTING_COUNT; i++) {
        ui_system_set_item_state(s_items[i], s_titles[i], s_values[i],
                                 s_indicators[i], i == s_selected, true);
    }

    lv_label_set_text_fmt(s_values[SETTING_BRIGHTNESS], "%u%%",
                          (unsigned)BRIGHTNESS_LEVELS[s_brightness_index]);
    lv_label_set_text_fmt(s_values[SETTING_HOUR], "%02u", (unsigned)hour);
    lv_label_set_text_fmt(s_values[SETTING_MINUTE], "%02u", (unsigned)minute);
    lv_label_set_text_fmt(s_values[SETTING_SECOND], "%02u", (unsigned)second);
    lv_label_set_text(s_values[SETTING_TIME_FORMAT],
                      ui_status_get_time_format() == UI_STATUS_TIME_HH_MM_SS
                          ? "HH:MM:SS" : "HH:MM");
    lv_label_set_text(s_values[SETTING_WIFI], wifi_state_text());
    lv_label_set_text(s_values[SETTING_RESET], "100%");
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
    lv_label_set_text(s_wifi_action_values[WIFI_ACTION_SETUP], "");
    lv_label_set_text(s_wifi_action_values[WIFI_ACTION_SYNC_TIME],
                      enabled ? time_sync_state_text() : "OFF");
    lv_label_set_text(s_wifi_action_values[WIFI_ACTION_BACK], "");
}

static void persist_settings(void)
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
    };
    app_settings_save(&settings);
}

static void restore_default_settings(void)
{
    if (app_settings_reset() != ESP_OK) return;

    const app_settings_t *settings = app_settings_get();
    s_brightness_index = settings->brightness_index;
    bsp_display_backlight(BRIGHTNESS_LEVELS[s_brightness_index]);
    ui_status_set_time(settings->hour, settings->minute, settings->second);
    ui_status_set_time_format(settings->time_format == APP_SETTINGS_TIME_HH_MM_SS
                                  ? UI_STATUS_TIME_HH_MM_SS : UI_STATUS_TIME_HH_MM);
    wifi_manager_set_enabled(settings->wifi_enabled);
}

static void refresh_timer(lv_timer_t *timer)
{
    (void)timer;
    if (s_view == SETTINGS_VIEW_MAIN) {
        settings_refresh();
    } else if (s_view == SETTINGS_VIEW_WIFI) {
        wifi_details_refresh();
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
    lv_obj_set_pos(state_label, 16, 91);
    s_wifi_state_value = ui_system_label(s_scr, "", &lv_font_montserrat_14,
                                         UI_SYSTEM_TEXT);
    lv_obj_set_width(s_wifi_state_value, 130);
    lv_obj_set_style_text_align(s_wifi_state_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_wifi_state_value, 94, 91);

    lv_obj_t *ssid_label = ui_system_label(s_scr, "SSID", &lv_font_montserrat_14,
                                           UI_SYSTEM_MUTED);
    lv_obj_set_pos(ssid_label, 16, 118);
    s_wifi_ssid_value = ui_system_label(s_scr, "", &lv_font_montserrat_14,
                                        UI_SYSTEM_TEXT);
    lv_obj_set_width(s_wifi_ssid_value, 160);
    lv_label_set_long_mode(s_wifi_ssid_value, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_wifi_ssid_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_wifi_ssid_value, 64, 118);

    static const char * const titles[WIFI_ACTION_COUNT] = {
        "Wi-Fi 开关",
        "重新配网",
        "同步时间",
        "返回",
    };
    for (size_t i = 0; i < WIFI_ACTION_COUNT; i++) {
        int y = 144 + (int)i * 37;
        s_wifi_actions[i] = ui_system_item_create(s_scr, 16, y, 208, 33);
        s_wifi_action_titles[i] = ui_system_label(s_wifi_actions[i], titles[i],
                                                   &ui_font_noto_sc_14, UI_SYSTEM_TEXT);
        lv_obj_set_pos(s_wifi_action_titles[i], 16, 9);
        s_wifi_action_values[i] = ui_system_label(s_wifi_actions[i], "",
                                                   &lv_font_montserrat_14, UI_SYSTEM_MUTED);
        lv_obj_set_width(s_wifi_action_values[i], 82);
        lv_obj_set_style_text_align(s_wifi_action_values[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_wifi_action_values[i], 82, 9);
        s_wifi_action_indicators[i] = ui_system_label(s_wifi_actions[i], ">",
                                                       &lv_font_montserrat_20, UI_SYSTEM_MUTED);
        lv_obj_set_pos(s_wifi_action_indicators[i], 180, 5);
    }

    lv_obj_t *hint = ui_system_label(s_scr, "NTP: UTC+8", &lv_font_montserrat_14,
                                     UI_SYSTEM_MUTED);
    lv_obj_set_width(hint, 208);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(hint, 16, 296);
    wifi_details_refresh();
    lv_screen_load(s_scr);
}

static void settings_build(void)
{
    if (s_view == SETTINGS_VIEW_WIFI) {
        wifi_details_build();
        return;
    }
    if (s_view == SETTINGS_VIEW_PROVISIONING) {
        provisioning_build();
        return;
    }

    s_scr = ui_system_screen_create();

    lv_obj_t *back = ui_system_label(s_scr, "<", &lv_font_montserrat_20,
                                     UI_SYSTEM_TEXT);
    lv_obj_set_pos(back, 18, 42);

    lv_obj_t *heading = ui_system_label(s_scr, "设置", &ui_font_noto_sc_20,
                                        UI_SYSTEM_TEXT);
    lv_obj_set_width(heading, 208);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(heading, 16, 42);
    ui_system_divider(s_scr, 16, 77, 208);

    static const char * const titles[SETTING_COUNT] = {
        "屏幕亮度",
        "小时",
        "分钟",
        "秒钟",
        "时间格式",
        "网络",
        "恢复默认",
    };

    for (size_t i = 0; i < SETTING_COUNT; i++) {
        int y = 82 + (int)i * 34;
        s_items[i] = ui_system_item_create(s_scr, 16, y, 208, 31);
        s_titles[i] = ui_system_label(s_items[i], titles[i], &ui_font_noto_sc_14,
                                      UI_SYSTEM_TEXT);
        lv_obj_set_pos(s_titles[i], 16, 8);

        s_values[i] = ui_system_label(s_items[i], "", &lv_font_montserrat_14,
                                      UI_SYSTEM_MUTED);
        lv_obj_set_width(s_values[i], 72);
        lv_obj_set_style_text_align(s_values[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_values[i], 96, 8);

        s_indicators[i] = ui_system_label(s_items[i], ">", &lv_font_montserrat_20,
                                          UI_SYSTEM_MUTED);
        lv_obj_set_pos(s_indicators[i], 180, 4);
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
    clear_wifi_objects();
    s_view = view;
    settings_build();
}

void demo_settings_enter(void)
{
    s_brightness_index = app_settings_get()->brightness_index;
    bsp_display_backlight(BRIGHTNESS_LEVELS[s_brightness_index]);
    s_selected = 0;
    s_wifi_selected = 0;
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
    clear_wifi_objects();
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

    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    ui_status_get_time(&hour, &minute, &second);

    switch ((setting_id_t)s_selected) {
    case SETTING_BRIGHTNESS:
        s_brightness_index = (s_brightness_index + 1) %
                             (sizeof(BRIGHTNESS_LEVELS) / sizeof(BRIGHTNESS_LEVELS[0]));
        bsp_display_backlight(BRIGHTNESS_LEVELS[s_brightness_index]);
        persist_settings();
        break;
    case SETTING_HOUR:
        ui_status_set_time((hour + 1) % 24, minute, second);
        persist_settings();
        break;
    case SETTING_MINUTE:
        ui_status_set_time(hour, (minute + 1) % 60, second);
        persist_settings();
        break;
    case SETTING_SECOND:
        ui_status_set_time(hour, minute, (second + 1) % 60);
        persist_settings();
        break;
    case SETTING_TIME_FORMAT:
        ui_status_set_time_format(ui_status_get_time_format() == UI_STATUS_TIME_HH_MM
                                      ? UI_STATUS_TIME_HH_MM_SS : UI_STATUS_TIME_HH_MM);
        persist_settings();
        break;
    case SETTING_WIFI:
        if (wifi_manager_get_state() == WIFI_MANAGER_PROVISIONING) {
            show_view(SETTINGS_VIEW_PROVISIONING);
        } else {
            s_wifi_selected = 0;
            show_view(SETTINGS_VIEW_WIFI);
        }
        return;
    case SETTING_RESET:
        restore_default_settings();
        break;
    }
    settings_refresh();
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
            persist_settings();
        }
        wifi_details_refresh();
        break;
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

void demo_settings_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK || s_view == SETTINGS_VIEW_PROVISIONING) return;
    if (s_view == SETTINGS_VIEW_WIFI) {
        wifi_settings_key(btn);
    } else {
        main_settings_key(btn);
    }
}
