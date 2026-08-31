// main/demo_builtin.c - 内置硬件测试子菜单（显示、按键、音频、电量）
#include "demo.h"
#include "ui_system.h"
#include "ui_status.h"
#include "ui_font_noto_sc_14.h"
#include "ui_font_noto_sc_20.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "lvgl.h"

typedef struct {
    const char *name;
    void (*enter)(void);
    void (*exit)(void);
    void (*key)(bsp_btn_t btn, bsp_btn_ev_t ev);
} builtin_subdemo_t;

static const builtin_subdemo_t SUBDEMOS[] = {
    { "显示", demo_display_enter, demo_display_exit, demo_display_key },
    { "按键", demo_button_enter, demo_button_exit, demo_button_key },
    { "音频", demo_audio_enter, demo_audio_exit, demo_audio_key },
    { "电量", demo_battery_enter, demo_battery_exit, demo_battery_key },
    { "动画", demo_anim_enter, demo_anim_exit, demo_anim_key },
};

#define SUBDEMO_COUNT (sizeof(SUBDEMOS) / sizeof(SUBDEMOS[0]))
#define BUILTIN_ITEM_COUNT (SUBDEMO_COUNT + 1) // +1 for "返回"

static lv_obj_t *s_scr;
static lv_obj_t *s_cards[BUILTIN_ITEM_COUNT];
static lv_obj_t *s_rows[BUILTIN_ITEM_COUNT];
static lv_obj_t *s_status[BUILTIN_ITEM_COUNT];
static lv_obj_t *s_indicators[BUILTIN_ITEM_COUNT];
static int s_sel;
static int s_active_sub = -1;
static void (*s_on_exit)(void);

void demo_builtin_set_on_exit(void (*on_exit)(void))
{
    s_on_exit = on_exit;
}

static void builtin_menu_refresh(void)
{
    for (size_t i = 0; i < BUILTIN_ITEM_COUNT; i++) {
        ui_system_set_item_state(s_cards[i], s_rows[i], s_status[i],
                                 s_indicators[i], (int)i == s_sel, true);
    }
}

static void builtin_menu_build(void)
{
    s_scr = ui_system_screen_create();

    lv_obj_t *back = ui_system_label(s_scr, "<", &lv_font_montserrat_20, UI_SYSTEM_TEXT);
    lv_obj_set_pos(back, 18, 42);

    lv_obj_t *heading = ui_system_label(s_scr, "内置", &ui_font_noto_sc_20, UI_SYSTEM_TEXT);
    lv_obj_set_width(heading, 208);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(heading, 16, 42);
    ui_system_divider(s_scr, 16, 77, 208);

    for (size_t i = 0; i < SUBDEMO_COUNT; i++) {
        int y = 92 + (int)i * 32;
        s_cards[i] = ui_system_item_create(s_scr, 16, y, 208, 29);
        s_rows[i] = ui_system_label(s_cards[i], SUBDEMOS[i].name,
                                    &ui_font_noto_sc_14, UI_SYSTEM_TEXT);
        lv_obj_set_pos(s_rows[i], 16, 6);
        s_status[i] = ui_system_label(s_cards[i], "", &ui_font_noto_sc_14,
                                      UI_SYSTEM_MUTED);
        lv_obj_set_width(s_status[i], 56);
        lv_obj_set_style_text_align(s_status[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_status[i], 108, 6);
        s_indicators[i] = ui_system_label(s_cards[i], ">", &lv_font_montserrat_20,
                                           UI_SYSTEM_MUTED);
        lv_obj_set_pos(s_indicators[i], 180, 2);
    }

    // 返回选项
    int back_y = 92 + (int)SUBDEMO_COUNT * 32;
    s_cards[SUBDEMO_COUNT] = ui_system_item_create(s_scr, 16, back_y, 208, 29);
    s_rows[SUBDEMO_COUNT] = ui_system_label(s_cards[SUBDEMO_COUNT], "返回",
                                           &ui_font_noto_sc_14, UI_SYSTEM_TEXT);
    lv_obj_set_pos(s_rows[SUBDEMO_COUNT], 16, 6);
    s_status[SUBDEMO_COUNT] = ui_system_label(s_cards[SUBDEMO_COUNT], "", &ui_font_noto_sc_14,
                                             UI_SYSTEM_MUTED);
    lv_obj_set_width(s_status[SUBDEMO_COUNT], 56);
    lv_obj_set_style_text_align(s_status[SUBDEMO_COUNT], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_status[SUBDEMO_COUNT], 108, 6);
    s_indicators[SUBDEMO_COUNT] = ui_system_label(s_cards[SUBDEMO_COUNT], ">", &lv_font_montserrat_20,
                                                  UI_SYSTEM_MUTED);
    lv_obj_set_pos(s_indicators[SUBDEMO_COUNT], 180, 2);

    builtin_menu_refresh();
    lv_screen_load(s_scr);
    ui_status_set_visible(true);
}

void demo_builtin_enter(void)
{
    s_active_sub = -1;
    s_sel = 0;
    builtin_menu_build();
}

void demo_builtin_exit(void)
{
    if (s_active_sub >= 0) {
        SUBDEMOS[s_active_sub].exit();
        s_active_sub = -1;
    }
    ui_status_set_visible(false);
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    for (size_t i = 0; i < BUILTIN_ITEM_COUNT; i++) {
        s_cards[i] = NULL;
        s_rows[i] = NULL;
        s_status[i] = NULL;
        s_indicators[i] = NULL;
    }
}

bool demo_builtin_back(void)
{
    if (s_active_sub >= 0) {
        SUBDEMOS[s_active_sub].exit();
        s_active_sub = -1;
        builtin_menu_build();
        return true;
    }
    // 内置子菜单已移入设置页：长按返回时回到设置页而非主菜单。
    if (s_on_exit) {
        demo_builtin_exit();
        s_on_exit();
        return true;
    }
    return false;
}

void demo_builtin_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (s_active_sub >= 0) {
        SUBDEMOS[s_active_sub].key(btn, ev);
        return;
    }

    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP) {
        s_sel = (s_sel + BUILTIN_ITEM_COUNT - 1) % BUILTIN_ITEM_COUNT;
        builtin_menu_refresh();
        return;
    }

    if (btn == BSP_BTN_DOWN) {
        s_sel = (s_sel + 1) % BUILTIN_ITEM_COUNT;
        builtin_menu_refresh();
        return;
    }

    if (btn == BSP_BTN_OK) {
        if (s_sel < (int)SUBDEMO_COUNT) {
            s_active_sub = s_sel;
            ui_status_set_visible(false);
            if (s_scr) {
                lv_obj_delete(s_scr);
                s_scr = NULL;
            }
            for (size_t i = 0; i < BUILTIN_ITEM_COUNT; i++) {
                s_cards[i] = NULL;
                s_rows[i] = NULL;
                s_status[i] = NULL;
                s_indicators[i] = NULL;
            }
            SUBDEMOS[s_active_sub].enter();
        } else if (s_sel == (int)SUBDEMO_COUNT) {
            // 返回主菜单
            demo_builtin_exit();
            if (s_on_exit) {
                s_on_exit();
            }
        }
    }
}
