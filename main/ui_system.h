#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#define UI_SYSTEM_BG              0x0B0D11
#define UI_SYSTEM_SURFACE         0x191D24
#define UI_SYSTEM_SURFACE_ACTIVE  0xECE9DF
#define UI_SYSTEM_BORDER          0x303641
#define UI_SYSTEM_TEXT            0xF3F1EB
#define UI_SYSTEM_MUTED           0xA6ABB5
#define UI_SYSTEM_TEXT_ACTIVE     0x13161B
#define UI_SYSTEM_ACCENT          0xC6AA70
#define UI_SYSTEM_DISABLED        0x686E77

lv_obj_t *ui_system_screen_create(void);
lv_obj_t *ui_system_item_create(lv_obj_t *parent, int x, int y, int w, int h);
lv_obj_t *ui_system_label(lv_obj_t *parent, const char *text,
                          const lv_font_t *font, uint32_t color);
void ui_system_divider(lv_obj_t *parent, int x, int y, int width);
void ui_system_set_item_state(lv_obj_t *item, lv_obj_t *title, lv_obj_t *detail,
                              lv_obj_t *indicator, bool selected, bool enabled);
