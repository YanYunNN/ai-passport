// main/demo_chat.c —— Chat 语音助手入口。
// 完整流程（录音→ASR→AI→TTS→播放）随后实现；当前为占位页面。
#include "demo.h"
#include "ui_system.h"
#include "ui_font_noto_sc_14.h"
#include "ui_font_noto_sc_20.h"
#include "lvgl.h"

static lv_obj_t *s_scr;

void demo_chat_enter(void)
{
    s_scr = ui_system_screen_create();
    lv_obj_t *heading = ui_system_label(s_scr, "Chat", &ui_font_noto_sc_20, UI_SYSTEM_TEXT);
    lv_obj_set_width(heading, 208);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(heading, 16, 32);
    ui_system_divider(s_scr, 16, 60, 208);

    lv_obj_t *hint = ui_system_label(s_scr, "语音聊天（开发中）", &ui_font_noto_sc_14, UI_SYSTEM_MUTED);
    lv_obj_set_width(hint, 208);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(hint, 16, 148);
    lv_screen_load(s_scr);
}

void demo_chat_exit(void)
{
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
}

void demo_chat_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    (void)btn;
    (void)ev;
}

bool demo_chat_back(void)
{
    return false;
}
