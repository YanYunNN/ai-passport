#include "demo.h"
#include "bsp_display.h"
#include "ui_font_noto_sc_14.h"
#include "ui_font_noto_sc_20.h"
#include "ui_status.h"
#include "ui_system.h"
#include "lvgl.h"

#define SETTING_COUNT 2

static const uint8_t BRIGHTNESS_LEVELS[] = { 30, 60, 100 };

static lv_obj_t *s_scr;
static lv_obj_t *s_items[SETTING_COUNT];
static lv_obj_t *s_titles[SETTING_COUNT];
static lv_obj_t *s_values[SETTING_COUNT];
static lv_obj_t *s_indicators[SETTING_COUNT];
static uint8_t s_selected;
static uint8_t s_brightness_index = 2;

static void settings_refresh(void)
{
    for (size_t i = 0; i < SETTING_COUNT; i++) {
        ui_system_set_item_state(s_items[i], s_titles[i], s_values[i],
                                 s_indicators[i], i == s_selected, true);
    }

    lv_label_set_text_fmt(s_values[0], "%u%%",
                          (unsigned)BRIGHTNESS_LEVELS[s_brightness_index]);
    lv_label_set_text(s_values[1], "100%");
}

static void settings_build(void)
{
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
        "恢复默认",
    };

    for (size_t i = 0; i < SETTING_COUNT; i++) {
        int y = 94 + (int)i * 60;
        s_items[i] = ui_system_item_create(s_scr, 16, y, 208, 52);
        s_titles[i] = ui_system_label(s_items[i], titles[i], &ui_font_noto_sc_14,
                                      UI_SYSTEM_TEXT);
        lv_obj_set_pos(s_titles[i], 16, 17);

        s_values[i] = ui_system_label(s_items[i], "", &lv_font_montserrat_14,
                                      UI_SYSTEM_MUTED);
        lv_obj_set_width(s_values[i], 56);
        lv_obj_set_style_text_align(s_values[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_values[i], 118, 18);

        s_indicators[i] = ui_system_label(s_items[i], ">", &lv_font_montserrat_20,
                                          UI_SYSTEM_MUTED);
        lv_obj_set_pos(s_indicators[i], 180, 15);
    }

    settings_refresh();
    lv_screen_load(s_scr);
}

void demo_settings_enter(void)
{
    bsp_display_backlight(BRIGHTNESS_LEVELS[s_brightness_index]);
    s_selected = 0;
    settings_build();
    ui_status_set_visible(true);
}

void demo_settings_exit(void)
{
    ui_status_set_visible(false);
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    for (size_t i = 0; i < SETTING_COUNT; i++) {
        s_items[i] = NULL;
        s_titles[i] = NULL;
        s_values[i] = NULL;
        s_indicators[i] = NULL;
    }
}

void demo_settings_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP) {
        s_selected = (s_selected + SETTING_COUNT - 1) % SETTING_COUNT;
        settings_refresh();
    } else if (btn == BSP_BTN_DOWN) {
        s_selected = (s_selected + 1) % SETTING_COUNT;
        settings_refresh();
    } else if (btn == BSP_BTN_OK) {
        if (s_selected == 0) {
            s_brightness_index = (s_brightness_index + 1) %
                                 (sizeof(BRIGHTNESS_LEVELS) / sizeof(BRIGHTNESS_LEVELS[0]));
        } else {
            s_brightness_index = 2;
        }
        bsp_display_backlight(BRIGHTNESS_LEVELS[s_brightness_index]);
        settings_refresh();
    }
}
