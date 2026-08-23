#include "demo.h"
#include "bsp_display.h"
#include "ui_font_noto_sc_14.h"
#include "ui_font_noto_sc_20.h"
#include "ui_status.h"
#include "ui_system.h"
#include "wifi_manager.h"
#include "lvgl.h"

#define SETTING_COUNT 7

typedef enum {
    SETTING_BRIGHTNESS,
    SETTING_HOUR,
    SETTING_MINUTE,
    SETTING_SECOND,
    SETTING_TIME_FORMAT,
    SETTING_WIFI,
    SETTING_RESET,
} setting_id_t;

static const uint8_t BRIGHTNESS_LEVELS[] = { 30, 60, 100 };

static lv_obj_t *s_scr;
static lv_obj_t *s_items[SETTING_COUNT];
static lv_obj_t *s_titles[SETTING_COUNT];
static lv_obj_t *s_values[SETTING_COUNT];
static lv_obj_t *s_indicators[SETTING_COUNT];
static uint8_t s_selected;
static uint8_t s_brightness_index = 2;
static bool s_provisioning_view;

static const char *wifi_state_text(void)
{
    switch (wifi_manager_get_state()) {
    case WIFI_MANAGER_CONNECTED: return "ONLINE";
    case WIFI_MANAGER_CONNECTING: return "WAIT";
    case WIFI_MANAGER_PROVISIONING: return "SETUP";
    case WIFI_MANAGER_FAILED: return "RETRY";
    case WIFI_MANAGER_UNCONFIGURED: return "SETUP";
    default: return "OFF";
    }
}

static void settings_refresh(void)
{
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

static void provisioning_build(void)
{
    s_scr = ui_system_screen_create();

    lv_obj_t *heading = ui_system_label(s_scr, "WI-FI SETUP", &lv_font_montserrat_20,
                                        UI_SYSTEM_TEXT);
    lv_obj_set_width(heading, 208);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(heading, 16, 42);
    ui_system_divider(s_scr, 16, 77, 208);

    const char *labels[] = { "AP", "PASS", "OPEN" };
    const char *values[] = {
        wifi_manager_get_provisioning_ssid(),
        wifi_manager_get_provisioning_password(),
        "192.168.4.1",
    };
    for (size_t i = 0; i < 3; i++) {
        int y = 96 + (int)i * 48;
        lv_obj_t *label = ui_system_label(s_scr, labels[i], &lv_font_montserrat_14,
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

static void settings_build(void)
{
    if (s_provisioning_view) {
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

static void clear_settings_objects(void)
{
    for (size_t i = 0; i < SETTING_COUNT; i++) {
        s_items[i] = NULL;
        s_titles[i] = NULL;
        s_values[i] = NULL;
        s_indicators[i] = NULL;
    }
}

void demo_settings_enter(void)
{
    bsp_display_backlight(BRIGHTNESS_LEVELS[s_brightness_index]);
    s_selected = 0;
    s_provisioning_view = false;
    settings_build();
    ui_status_set_visible(true);
}

void demo_settings_exit(void)
{
    wifi_manager_stop_provisioning();
    ui_status_set_visible(false);
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    clear_settings_objects();
    s_provisioning_view = false;
}

void demo_settings_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK || s_provisioning_view) return;

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
        break;
    case SETTING_HOUR:
        ui_status_set_time((hour + 1) % 24, minute, second);
        break;
    case SETTING_MINUTE:
        ui_status_set_time(hour, (minute + 1) % 60, second);
        break;
    case SETTING_SECOND:
        ui_status_set_time(hour, minute, (second + 1) % 60);
        break;
    case SETTING_TIME_FORMAT:
        ui_status_set_time_format(ui_status_get_time_format() == UI_STATUS_TIME_HH_MM
                                      ? UI_STATUS_TIME_HH_MM_SS : UI_STATUS_TIME_HH_MM);
        break;
    case SETTING_WIFI:
        if (wifi_manager_start_provisioning() == ESP_OK) {
            s_provisioning_view = true;
            lv_obj_delete(s_scr);
            s_scr = NULL;
            clear_settings_objects();
            settings_build();
        }
        return;
    case SETTING_RESET:
        s_brightness_index = 2;
        bsp_display_backlight(BRIGHTNESS_LEVELS[s_brightness_index]);
        ui_status_set_time(0, 0, 0);
        ui_status_set_time_format(UI_STATUS_TIME_HH_MM);
        break;
    }
    settings_refresh();
}
