#include "ui_status.h"
#include "bsp_battery.h"
#include "ui_system.h"
#include "wifi_manager.h"
#include "lvgl.h"
#include <stdio.h>

#define STATUS_BATTERY_HEALTHY 0x45D483
#define STATUS_BATTERY_LOW     0xF05252
#define STATUS_WIFI_CONNECTED  0x45D483
#define STATUS_WIFI_ACTIVITY   0xC6AA70

static lv_obj_t *s_bar;
static lv_obj_t *s_time;
static lv_obj_t *s_soc;
static lv_obj_t *s_wifi_bars[3];
static lv_obj_t *s_battery_outline;
static lv_obj_t *s_battery_cap;
static lv_obj_t *s_battery_fill;
static lv_obj_t *s_charge_bolt[3];
static lv_timer_t *s_timer;
static char s_time_text[9];
static char s_soc_text[5];
static uint32_t s_last_battery_tick;
static uint32_t s_clock_seconds;
static uint32_t s_clock_base_tick;
static ui_status_time_format_t s_time_format = UI_STATUS_TIME_HH_MM;
static bool s_charging;

static lv_obj_t *block(lv_obj_t *parent, int x, int y, int w, int h,
                       uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static uint32_t clock_seconds_now(void)
{
    return (s_clock_seconds + (lv_tick_get() - s_clock_base_tick) / 1000u) % 86400u;
}

static void write_two_digits(char *text, uint32_t value)
{
    value %= 100u;
    text[0] = (char)('0' + value / 10u);
    text[1] = (char)('0' + value % 10u);
}

static void refresh_time(void)
{
    uint32_t total_seconds = clock_seconds_now();
    uint32_t hour = total_seconds / 3600u;
    uint32_t minute = (total_seconds / 60u) % 60u;
    uint32_t second = total_seconds % 60u;

    write_two_digits(&s_time_text[0], hour);
    s_time_text[2] = ':';
    write_two_digits(&s_time_text[3], minute);
    if (s_time_format == UI_STATUS_TIME_HH_MM_SS) {
        s_time_text[5] = ':';
        write_two_digits(&s_time_text[6], second);
        s_time_text[8] = '\0';
    } else {
        s_time_text[5] = '\0';
    }
    lv_label_set_text_static(s_time, s_time_text);
}

static void refresh_wifi(void)
{
    wifi_manager_state_t state = wifi_manager_get_state();
    uint32_t color = UI_SYSTEM_DISABLED;
    size_t active_bars = 0;

    if (state == WIFI_MANAGER_CONNECTED) {
        color = STATUS_WIFI_CONNECTED;
        active_bars = 3;
    } else if (state == WIFI_MANAGER_CONNECTING || state == WIFI_MANAGER_PROVISIONING) {
        color = STATUS_WIFI_ACTIVITY;
        active_bars = 1;
    }
    for (size_t i = 0; i < sizeof(s_wifi_bars) / sizeof(s_wifi_bars[0]); i++) {
        lv_obj_set_style_bg_color(s_wifi_bars[i],
                                  lv_color_hex(i < active_bars ? color : UI_SYSTEM_DISABLED), 0);
    }
}

static void set_battery_color(uint32_t color)
{
    lv_obj_set_style_text_color(s_soc, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(s_battery_outline, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(s_battery_cap, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(s_battery_fill, lv_color_hex(color), 0);
}

static void refresh_charge_icon(void)
{
    for (size_t i = 0; i < sizeof(s_charge_bolt) / sizeof(s_charge_bolt[0]); i++) {
        if (s_charging) {
            lv_obj_remove_flag(s_charge_bolt[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_charge_bolt[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void status_refresh(lv_timer_t *timer)
{
    (void)timer;
    refresh_time();
    refresh_wifi();
    refresh_charge_icon();

    uint32_t tick = lv_tick_get();
    if (s_last_battery_tick != 0 && tick - s_last_battery_tick < 5000u) return;
    s_last_battery_tick = tick;

    int soc = bsp_battery_soc();
    if (soc < 0) {
        snprintf(s_soc_text, sizeof(s_soc_text), "--%%");
        lv_obj_set_width(s_battery_fill, 0);
        set_battery_color(UI_SYSTEM_DISABLED);
    } else {
        if (soc > 100) soc = 100;
        snprintf(s_soc_text, sizeof(s_soc_text), "%d%%", soc);
        lv_obj_set_width(s_battery_fill, (soc * 12) / 100);
        set_battery_color(soc <= 20 ? STATUS_BATTERY_LOW : STATUS_BATTERY_HEALTHY);
    }
    lv_label_set_text_static(s_soc, s_soc_text);
}

void ui_status_init(void)
{
    if (s_bar) return;

    s_clock_base_tick = lv_tick_get();
    s_bar = block(lv_layer_top(), 0, 0, 240, 27, UI_SYSTEM_BG);
    block(s_bar, 0, 26, 240, 1, UI_SYSTEM_BORDER);

    s_time = ui_system_label(s_bar, "00:00", &lv_font_montserrat_14,
                             UI_SYSTEM_TEXT);
    lv_obj_set_pos(s_time, 10, 6);

    s_wifi_bars[0] = block(s_bar, 132, 16, 2, 3, UI_SYSTEM_DISABLED);
    s_wifi_bars[1] = block(s_bar, 136, 13, 2, 6, UI_SYSTEM_DISABLED);
    s_wifi_bars[2] = block(s_bar, 140, 10, 2, 9, UI_SYSTEM_DISABLED);

    /* 蓝牙协议栈未初始化，显示灰色 BT 标识而非连接状态。 */
    lv_obj_t *bluetooth = ui_system_label(s_bar, "BT", &lv_font_montserrat_14,
                                          UI_SYSTEM_DISABLED);
    lv_obj_set_pos(bluetooth, 151, 6);

    s_soc = ui_system_label(s_bar, "--%", &lv_font_montserrat_14,
                            UI_SYSTEM_DISABLED);
    lv_obj_set_width(s_soc, 36);
    lv_obj_set_style_text_align(s_soc, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_soc, 174, 6);

    s_battery_outline = block(s_bar, 214, 8, 16, 9, UI_SYSTEM_BG);
    lv_obj_set_style_border_width(s_battery_outline, 1, 0);
    lv_obj_set_style_border_color(s_battery_outline,
                                  lv_color_hex(UI_SYSTEM_DISABLED), 0);
    s_battery_cap = block(s_bar, 231, 10, 2, 5, UI_SYSTEM_DISABLED);
    s_battery_fill = block(s_bar, 216, 10, 0, 5, UI_SYSTEM_DISABLED);
    lv_obj_set_style_radius(s_battery_fill, 1, 0);

    s_charge_bolt[0] = block(s_bar, 222, 9, 3, 2, UI_SYSTEM_BG);
    s_charge_bolt[1] = block(s_bar, 220, 11, 3, 2, UI_SYSTEM_BG);
    s_charge_bolt[2] = block(s_bar, 223, 13, 3, 2, UI_SYSTEM_BG);
    refresh_charge_icon();

    s_timer = lv_timer_create(status_refresh, 1000, NULL);
    ui_status_set_visible(false);
}

void ui_status_set_visible(bool visible)
{
    if (!s_bar) return;

    if (visible) {
        lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        s_last_battery_tick = 0;
        status_refresh(NULL);
    } else {
        lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_status_set_time(uint8_t hour, uint8_t minute, uint8_t second)
{
    s_clock_seconds = ((uint32_t)(hour % 24u) * 3600u) +
                      ((uint32_t)(minute % 60u) * 60u) + (second % 60u);
    s_clock_base_tick = lv_tick_get();
    if (s_bar) status_refresh(NULL);
}

void ui_status_get_time(uint8_t *hour, uint8_t *minute, uint8_t *second)
{
    uint32_t total_seconds = clock_seconds_now();
    if (hour) *hour = (uint8_t)(total_seconds / 3600u);
    if (minute) *minute = (uint8_t)((total_seconds / 60u) % 60u);
    if (second) *second = (uint8_t)(total_seconds % 60u);
}

void ui_status_set_time_format(ui_status_time_format_t format)
{
    s_time_format = format == UI_STATUS_TIME_HH_MM_SS ? UI_STATUS_TIME_HH_MM_SS
                                                       : UI_STATUS_TIME_HH_MM;
    if (s_bar) status_refresh(NULL);
}

ui_status_time_format_t ui_status_get_time_format(void)
{
    return s_time_format;
}

void ui_status_set_charging(bool charging)
{
    s_charging = charging;
    if (s_bar) status_refresh(NULL);
}
