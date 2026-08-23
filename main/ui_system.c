#include "ui_system.h"

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

lv_obj_t *ui_system_screen_create(void)
{
    lv_obj_t *screen = block(NULL, 0, 0, 240, 320, UI_SYSTEM_BG);
    lv_obj_set_style_radius(screen, 0, 0);
    return screen;
}

lv_obj_t *ui_system_item_create(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *item = block(parent, x, y, w, h, UI_SYSTEM_SURFACE);
    lv_obj_set_style_radius(item, 10, 0);
    lv_obj_set_style_border_width(item, 1, 0);
    lv_obj_set_style_border_color(item, lv_color_hex(UI_SYSTEM_BORDER), 0);
    return item;
}

lv_obj_t *ui_system_label(lv_obj_t *parent, const char *text,
                          const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

void ui_system_divider(lv_obj_t *parent, int x, int y, int width)
{
    block(parent, x, y, width, 1, UI_SYSTEM_BORDER);
}

void ui_system_set_item_state(lv_obj_t *item, lv_obj_t *title, lv_obj_t *detail,
                              lv_obj_t *indicator, bool selected, bool enabled)
{
    uint32_t surface = UI_SYSTEM_SURFACE;
    uint32_t border = UI_SYSTEM_BORDER;
    uint32_t primary = UI_SYSTEM_TEXT;
    uint32_t secondary = UI_SYSTEM_MUTED;

    if (!enabled) {
        surface = 0x15181D;
        border = 0x242932;
        primary = UI_SYSTEM_DISABLED;
        secondary = UI_SYSTEM_DISABLED;
    } else if (selected) {
        surface = UI_SYSTEM_SURFACE_ACTIVE;
        border = UI_SYSTEM_ACCENT;
        primary = UI_SYSTEM_TEXT_ACTIVE;
        secondary = 0x50555D;
    }

    lv_obj_set_style_bg_color(item, lv_color_hex(surface), 0);
    lv_obj_set_style_border_color(item, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(item, selected && enabled ? 2 : 1, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(primary), 0);

    if (detail) {
        lv_obj_set_style_text_color(detail, lv_color_hex(secondary), 0);
    }
    if (indicator) {
        if (enabled) {
            lv_obj_remove_flag(indicator, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(indicator, lv_color_hex(secondary), 0);
        } else {
            lv_obj_add_flag(indicator, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
