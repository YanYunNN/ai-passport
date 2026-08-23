// main/main.c - FoloToy-Card BSP 驱动参考示例:初始化 + 菜单 + 按键分发。
//
// 按键语义(全局统一):
//   上/下 短按   菜单中=移动选中项;演示页中=该页自定义
//   确定  短按   菜单中=进入选中项;演示页中=该页自定义
//   确定  长按   演示页中=返回菜单(由本文件统一拦截)
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"
#include "demo.h"
#include "ui_font_noto_sc_14.h"
#include "ui_font_noto_sc_20.h"
#include "ui_status.h"
#include "ui_system.h"
#include "wifi_manager.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "main";

static const demo_entry_t DEMOS[] = {
    { "显示", demo_display_enter, demo_display_exit, demo_display_key },
    { "按键", demo_button_enter, demo_button_exit, demo_button_key },
    { "音频", demo_audio_enter, demo_audio_exit, demo_audio_key },
    { "电量", demo_battery_enter, demo_battery_exit, demo_battery_key },
    { "阅读", demo_reader_enter, demo_reader_exit, demo_reader_key },
    { "设置", demo_settings_enter, demo_settings_exit, demo_settings_key },
};
#define DEMO_COUNT (sizeof(DEMOS) / sizeof(DEMOS[0]))

static bool s_ok[DEMO_COUNT];
static lv_obj_t *s_menu_scr;
static lv_obj_t *s_cards[DEMO_COUNT];
static lv_obj_t *s_rows[DEMO_COUNT];
static lv_obj_t *s_status[DEMO_COUNT];
static lv_obj_t *s_indicators[DEMO_COUNT];
static int s_sel;
static int s_active = -1;

static void menu_refresh(void)
{
    for (size_t i = 0; i < DEMO_COUNT; i++) {
        lv_label_set_text(s_status[i], s_ok[i] ? "" : "不可用");
        ui_system_set_item_state(s_cards[i], s_rows[i], s_status[i],
                                 s_indicators[i], (int)i == s_sel, s_ok[i]);
    }
}

static void menu_build(void)
{
    s_menu_scr = ui_system_screen_create();

    lv_obj_t *heading = ui_system_label(s_menu_scr, "主菜单", &ui_font_noto_sc_20,
                                        UI_SYSTEM_TEXT);
    lv_obj_set_width(heading, 208);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(heading, 16, 35);
    ui_system_divider(s_menu_scr, 16, 66, 208);

    for (size_t i = 0; i < DEMO_COUNT; i++) {
        int y = 73 + (int)i * 39;
        s_cards[i] = ui_system_item_create(s_menu_scr, 16, y, 208, 36);

        s_rows[i] = ui_system_label(s_cards[i], DEMOS[i].name,
                                    &ui_font_noto_sc_14, UI_SYSTEM_TEXT);
        lv_obj_set_pos(s_rows[i], 16, 10);

        s_status[i] = ui_system_label(s_cards[i], "", &ui_font_noto_sc_14,
                                      UI_SYSTEM_MUTED);
        lv_obj_set_width(s_status[i], 56);
        lv_obj_set_style_text_align(s_status[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_status[i], 108, 10);

        s_indicators[i] = ui_system_label(s_cards[i], ">", &lv_font_montserrat_20,
                                           UI_SYSTEM_MUTED);
        lv_obj_set_pos(s_indicators[i], 180, 7);
    }

    menu_refresh();
    lv_screen_load(s_menu_scr);
}

static void enter_menu(void)
{
    s_active = -1;
    menu_build();
    ui_status_set_visible(true);
}

// 按键回调运行在 button 组件的任务里,操作 LVGL 必须加锁。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!bsp_lvgl_lock(500)) return;

    if (s_active >= 0) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            DEMOS[s_active].exit();
            enter_menu();
        } else {
            DEMOS[s_active].key(btn, ev);
        }
    } else if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP) {
            s_sel = (s_sel + DEMO_COUNT - 1) % DEMO_COUNT;
            menu_refresh();
        }
        if (btn == BSP_BTN_DOWN) {
            s_sel = (s_sel + 1) % DEMO_COUNT;
            menu_refresh();
        }
        if (btn == BSP_BTN_OK && s_ok[s_sel]) {
            s_active = s_sel;
            ui_status_set_visible(false);
            lv_obj_delete(s_menu_scr);
            s_menu_scr = NULL;
            for (size_t i = 0; i < DEMO_COUNT; i++) {
                s_cards[i] = NULL;
                s_rows[i] = NULL;
                s_status[i] = NULL;
                s_indicators[i] = NULL;
            }
            DEMOS[s_active].enter();
        }
    }
    bsp_lvgl_unlock();
}

void app_main(void)
{
    ESP_LOGI(TAG, "FoloToy-Card BSP demo 启动");

    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,demo 无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    s_ok[0] = true;
    s_ok[1] = (bsp_button_init(on_key, NULL) == ESP_OK);
    s_ok[2] = (bsp_audio_init() == ESP_OK);
    s_ok[3] = (bsp_battery_init() == ESP_OK);
    s_ok[4] = true;
    s_ok[5] = true;

    esp_err_t wifi_result = wifi_manager_init();
    if (wifi_result != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi 初始化失败: %s", esp_err_to_name(wifi_result));
    }

    if (bsp_lvgl_lock(1000)) {
        ui_status_init();
        enter_menu();
        bsp_lvgl_unlock();
    }

    ESP_LOGI(TAG, "就绪:显示=%d 按键=%d 音频=%d 电量=%d 阅读=%d 设置=%d Wi-Fi=%d",
             s_ok[0], s_ok[1], s_ok[2], s_ok[3], s_ok[4], s_ok[5],
             wifi_result == ESP_OK);
}
