#include "ui_status.h"
#include "bsp_battery.h"
#include "ui_system.h"
#include "lvgl.h"
#include <stdio.h>

static lv_obj_t *s_bar;
static lv_obj_t *s_time;
static lv_obj_t *s_soc;
static lv_obj_t *s_battery_fill;
static lv_timer_t *s_timer;
static char s_time_text[6];
static char s_soc_text[5];
static uint32_t s_last_battery_tick;

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

static void status_refresh(lv_timer_t *timer)
{
    (void)timer;
    uint32_t tick = lv_tick_get();
    uint32_t elapsed_seconds = tick / 1000u;
    uint32_t minutes = elapsed_seconds / 60u;
    uint32_t seconds = elapsed_seconds % 60u;

    snprintf(s_time_text, sizeof(s_time_text), "%02u:%02u",
             (unsigned)(minutes % 100u), (unsigned)seconds);
    lv_label_set_text_static(s_time, s_time_text);

    if (s_last_battery_tick != 0 && tick - s_last_battery_tick < 5000u) return;
    s_last_battery_tick = tick;

    int soc = bsp_battery_soc();
    if (soc < 0) {
        snprintf(s_soc_text, sizeof(s_soc_text), "--%%");
        lv_obj_set_width(s_battery_fill, 0);
    } else {
        /* 防御性限制：异常 SOC 也不会撑破状态栏文本或电量图标。 */
        if (soc > 100) soc = 100;
        snprintf(s_soc_text, sizeof(s_soc_text), "%d%%", soc);
        lv_obj_set_width(s_battery_fill, (soc * 12) / 100);
    }
    lv_label_set_text_static(s_soc, s_soc_text);
}

void ui_status_init(void)
{
    if (s_bar) return;

    s_bar = block(lv_layer_top(), 0, 0, 240, 27, UI_SYSTEM_BG);
    block(s_bar, 0, 26, 240, 1, UI_SYSTEM_BORDER);

    s_time = ui_system_label(s_bar, "00:00", &lv_font_montserrat_14,
                             UI_SYSTEM_TEXT);
    lv_obj_set_pos(s_time, 10, 6);

    /* Wi-Fi 未初始化时以灰色信号条表示未连接。 */
    block(s_bar, 132, 16, 2, 3, UI_SYSTEM_DISABLED);
    block(s_bar, 136, 13, 2, 6, UI_SYSTEM_DISABLED);
    block(s_bar, 140, 10, 2, 9, UI_SYSTEM_DISABLED);

    /* 蓝牙协议栈未初始化，显示灰色 BT 标识而非连接状态。 */
    lv_obj_t *bluetooth = ui_system_label(s_bar, "BT", &lv_font_montserrat_14,
                                          UI_SYSTEM_DISABLED);
    lv_obj_set_pos(bluetooth, 151, 6);

    s_soc = ui_system_label(s_bar, "--%", &lv_font_montserrat_14,
                            UI_SYSTEM_TEXT);
    lv_obj_set_width(s_soc, 36);
    lv_obj_set_style_text_align(s_soc, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_soc, 174, 6);

    lv_obj_t *battery = block(s_bar, 214, 8, 16, 9, UI_SYSTEM_BG);
    lv_obj_set_style_border_width(battery, 1, 0);
    lv_obj_set_style_border_color(battery, lv_color_hex(UI_SYSTEM_TEXT), 0);
    block(s_bar, 231, 10, 2, 5, UI_SYSTEM_TEXT);

    s_battery_fill = block(s_bar, 216, 10, 0, 5, UI_SYSTEM_ACCENT);
    lv_obj_set_style_radius(s_battery_fill, 1, 0);

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
