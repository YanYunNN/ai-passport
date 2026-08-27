// main/demo_image.c - 云端推送图片画廊展示页面。
#include "demo.h"
#include "kiro_passport_network.h"
#include "ui_font_noto_sc_14.h"
#include "ui_font_noto_sc_20.h"
#include "ui_status.h"
#include "ui_system.h"
#include "lvgl.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "demo_image";

static lv_obj_t *s_scr;
static lv_obj_t *s_img_obj;
static lv_obj_t *s_placeholder;
static lv_obj_t *s_info_bar;
static lv_obj_t *s_info_label;
static lv_image_dsc_t s_img_dsc;
static lv_timer_t *s_refresh_timer;
static uint32_t s_current_version = 0;
static bool s_show_info = true;

static void render_current_image(void)
{
    kiro_passport_image_info_t info;
    bool has_image = kiro_passport_network_get_image(&info);

    if (has_image && info.size > 0 && info.data != NULL) {
        if (s_placeholder) {
            lv_obj_add_flag(s_placeholder, LV_OBJ_FLAG_HIDDEN);
        }

        memset(&s_img_dsc, 0, sizeof(s_img_dsc));
        s_img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_img_dsc.header.cf = LV_COLOR_FORMAT_RAW;
        s_img_dsc.header.w = 240;
        s_img_dsc.header.h = 320;
        s_img_dsc.data_size = info.size;
        s_img_dsc.data = info.data;

        if (!s_img_obj) {
            s_img_obj = lv_image_create(s_scr);
            lv_obj_set_pos(s_img_obj, 0, 0);
            lv_obj_set_size(s_img_obj, 240, 320);
            lv_obj_set_style_bg_color(s_img_obj, lv_color_black(), 0);
        }

        if (info.size >= 4) {
            ESP_LOGI(TAG, "准备渲染图片: 魔数=[%02X %02X %02X %02X], size=%zu, title=%s, v=%lu",
                     info.data[0], info.data[1], info.data[2], info.data[3],
                     info.size, info.title, (unsigned long)info.version);
        }

        lv_obj_clear_flag(s_img_obj, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(s_img_obj, &s_img_dsc);
        lv_obj_invalidate(s_img_obj);

        if (!s_info_bar) {
            s_info_bar = lv_obj_create(s_scr);
            lv_obj_set_pos(s_info_bar, 0, 272);
            lv_obj_set_size(s_info_bar, 240, 48);
            lv_obj_set_style_bg_color(s_info_bar, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(s_info_bar, LV_OPA_70, 0);
            lv_obj_set_style_border_width(s_info_bar, 0, 0);
            lv_obj_set_style_radius(s_info_bar, 0, 0);
            lv_obj_set_style_pad_all(s_info_bar, 4, 0);
            lv_obj_clear_flag(s_info_bar, LV_OBJ_FLAG_SCROLLABLE);

            s_info_label = lv_label_create(s_info_bar);
            lv_obj_set_style_text_font(s_info_label, &ui_font_noto_sc_14, 0);
            lv_obj_set_style_text_color(s_info_label, lv_color_white(), 0);
            lv_obj_set_width(s_info_label, 232);
            lv_obj_set_pos(s_info_label, 4, 2);
        }

        if (s_info_bar) {
            lv_obj_move_foreground(s_info_bar);
        }

        if (s_info_label) {
            char desc[128];
            snprintf(desc, sizeof(desc), "%s (%u KB)\nOK: 切换信息栏",
                     info.title[0] ? info.title : "云端图片", (unsigned int)(info.size / 1024));
            lv_label_set_text(s_info_label, desc);
        }

        if (s_info_bar) {
            if (s_show_info) {
                lv_obj_clear_flag(s_info_bar, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_info_bar, LV_OBJ_FLAG_HIDDEN);
            }
        }

        ESP_LOGI(TAG, "渲染图片完成: size=%zu, title=%s, v=%lu", info.size, info.title, (unsigned long)info.version);
        s_current_version = info.version;
    } else {
        if (s_img_obj) {
            lv_obj_add_flag(s_img_obj, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_info_bar) {
            lv_obj_add_flag(s_info_bar, LV_OBJ_FLAG_HIDDEN);
        }

        if (!s_placeholder) {
            s_placeholder = lv_obj_create(s_scr);
            lv_obj_set_pos(s_placeholder, 16, 60);
            lv_obj_set_size(s_placeholder, 208, 220);
            lv_obj_set_style_bg_color(s_placeholder, lv_color_hex(0x161B22), 0);
            lv_obj_set_style_border_color(s_placeholder, lv_color_hex(0x30363D), 0);
            lv_obj_set_style_border_width(s_placeholder, 1, 0);
            lv_obj_set_style_radius(s_placeholder, 8, 0);
            lv_obj_clear_flag(s_placeholder, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *title = ui_system_label(s_placeholder, "暂无图片", &ui_font_noto_sc_14, UI_SYSTEM_TEXT);
            lv_obj_set_width(title, 192);
            lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(title, 0, 16);

            lv_obj_t *hint = ui_system_label(s_placeholder,
                "请在电脑或手机访问后台:\n\nws.yanyun.asia/admin\n\n在「图片工作台」选择\n图片并点击推送到设备",
                &ui_font_noto_sc_14, UI_SYSTEM_MUTED);
            lv_obj_set_width(hint, 192);
            lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(hint, 0, 60);
        }
        lv_obj_clear_flag(s_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_check_timer(lv_timer_t *timer)
{
    (void)timer;
    kiro_passport_image_info_t info;
    if (kiro_passport_network_get_image(&info)) {
        if (info.version != s_current_version) {
            ESP_LOGI(TAG, "检测到新图片版本 (v%lu)，正在刷新屏幕", (unsigned long)info.version);
            render_current_image();
        }
    }
}

void demo_image_enter(void)
{
    s_scr = ui_system_screen_create();
    s_placeholder = NULL;
    s_img_obj = NULL;
    s_info_bar = NULL;
    s_info_label = NULL;
    s_show_info = true;
    s_current_version = 0;

    render_current_image();

    s_refresh_timer = lv_timer_create(on_check_timer, 500, NULL);
    lv_screen_load(s_scr);
}

void demo_image_exit(void)
{
    if (s_refresh_timer) {
        lv_timer_delete(s_refresh_timer);
        s_refresh_timer = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_img_obj = NULL;
        s_placeholder = NULL;
        s_info_bar = NULL;
        s_info_label = NULL;
    }
}

void demo_image_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_OK) {
        s_show_info = !s_show_info;
        if (s_info_bar) {
            if (s_show_info) {
                lv_obj_clear_flag(s_info_bar, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_info_bar, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        render_current_image();
    }
}
