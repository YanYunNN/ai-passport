// main/main.c - FoloToy-Card BSP 驱动参考示例:初始化 + 菜单 + 按键分发。
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"
#include "app_settings.h"
#include "debug_log.h"
#include "power_manager.h"
#include "screencast.h"
#include "demo.h"
#include "kiro_passport_network.h"
#include "ui_font_noto_sc_14.h"
#include "ui_font_noto_sc_20.h"
#include "ui_status.h"
#include "ui_system.h"
#include "wifi_manager.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "main";

static const demo_entry_t DEMOS[] = {
    { "阅读", demo_reader_enter, demo_reader_exit, demo_reader_key, NULL },
    { "图片", demo_image_enter, demo_image_exit, demo_image_key, NULL },
    { "游戏", demo_games_enter, demo_games_exit, demo_games_key, demo_games_back },
    { "Kiro", demo_kiro_passport_enter, demo_kiro_passport_exit, demo_kiro_passport_key, NULL },
    { "Chat", demo_chat_enter, demo_chat_exit, demo_chat_key, demo_chat_back },
    { "设置", demo_settings_enter, demo_settings_exit, demo_settings_key, demo_settings_back },
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

static lv_timer_t *s_menu_timer;

static void menu_refresh(void)
{
    bool has_unread = kiro_passport_network_has_unread_notify();

    for (size_t i = 0; i < DEMO_COUNT; i++) {
        const char *status_str = s_ok[i] ? "" : "不可用";
        if (i == 3 && s_ok[3] && has_unread) {
            status_str = "● 新消息";
        }
        lv_label_set_text(s_status[i], status_str);
        ui_system_set_item_state(s_cards[i], s_rows[i], s_status[i],
                                 s_indicators[i], (int)i == s_sel, s_ok[i]);
        if (i == 3 && s_ok[3] && has_unread) {
            lv_obj_set_style_text_color(s_status[i], lv_color_hex(UI_SYSTEM_ACCENT), 0);
        }
    }
}

static void menu_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_active < 0 && s_menu_scr) {
        static bool s_last_has_unread = false;
        bool has_unread = kiro_passport_network_has_unread_notify();
        if (has_unread != s_last_has_unread) {
            s_last_has_unread = has_unread;
            menu_refresh();
        }
    }
}

static void menu_build(void)
{
    s_menu_scr = ui_system_screen_create();

    lv_obj_t *heading = ui_system_label(s_menu_scr, "主菜单", &ui_font_noto_sc_20,
                                        UI_SYSTEM_TEXT);
    lv_obj_set_width(heading, 208);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(heading, 16, 32);
    ui_system_divider(s_menu_scr, 16, 60, 208);

    for (size_t i = 0; i < DEMO_COUNT; i++) {
        int y = 66 + (int)i * 41;
        s_cards[i] = ui_system_item_create(s_menu_scr, 16, y, 208, 36);
        s_rows[i] = ui_system_label(s_cards[i], DEMOS[i].name,
                                    &ui_font_noto_sc_14, UI_SYSTEM_TEXT);
        lv_obj_set_pos(s_rows[i], 16, 10);
        s_status[i] = ui_system_label(s_cards[i], "", &ui_font_noto_sc_14,
                                      UI_SYSTEM_MUTED);
        lv_obj_set_width(s_status[i], 68);
        lv_obj_set_style_text_align(s_status[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_status[i], 98, 10);
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
    if (!s_menu_timer) {
        s_menu_timer = lv_timer_create(menu_timer_cb, 500, NULL);
    }
}

/* The callback runs in the button task, so all LVGL access is mutex-protected. */
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    bool was_dimmed = power_manager_activity_notify();
    if (was_dimmed) {
        // 息屏状态下按任意键仅点亮屏幕，不触发操作
        return;
    }
    if (!bsp_lvgl_lock(500)) return;

    if (s_active >= 0) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            if (DEMOS[s_active].back && DEMOS[s_active].back()) {
                // Handled internally by sub-level navigation
            } else {
                DEMOS[s_active].exit();
                enter_menu();
            }
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
            if (s_menu_timer) {
                lv_timer_delete(s_menu_timer);
                s_menu_timer = NULL;
            }
            s_active = s_sel;
            ui_status_set_visible(false);
            lv_obj_t *old_scr = s_menu_scr;
            s_menu_scr = NULL;
            for (size_t i = 0; i < DEMO_COUNT; i++) {
                s_cards[i] = NULL;
                s_rows[i] = NULL;
                s_status[i] = NULL;
                s_indicators[i] = NULL;
            }
            DEMOS[s_active].enter();
            if (old_scr) {
                lv_obj_delete(old_scr);
            }
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

    esp_err_t settings_result = app_settings_init();
    if (settings_result != ESP_OK) {
        ESP_LOGW(TAG, "应用设置初始化失败: %s", esp_err_to_name(settings_result));
    }
    const app_settings_t *settings = app_settings_get();
    debug_log_init(settings->debug_enabled);
    esp_err_t power_result = power_manager_init(settings->light_sleep_enabled);
    if (power_result != ESP_OK) {
        ESP_LOGW(TAG, "浅睡眠策略未生效: %s", esp_err_to_name(power_result));
    }

    bool button_ok = (bsp_button_init(on_key, NULL) == ESP_OK);
    bool audio_ok = (bsp_audio_init() == ESP_OK);
    bool batt_ok = (bsp_battery_init() == ESP_OK);

    s_ok[0] = true; // 阅读
    s_ok[1] = true; // 图片
    s_ok[2] = true; // 游戏
    s_ok[3] = true; // Kiro (will update below)
    s_ok[4] = true; // Chat (requires relay enrollment; see below)
    s_ok[5] = true; // 设置（内置硬件测试已移入设置页）

    demo_games_set_on_exit(enter_menu);

    esp_err_t wifi_enable_result = wifi_manager_set_enabled(settings->wifi_enabled);
    if (wifi_enable_result != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi 开关设置失败: %s", esp_err_to_name(wifi_enable_result));
    }
    esp_err_t wifi_result = wifi_manager_init();
    if (wifi_result != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi 初始化失败: %s", esp_err_to_name(wifi_result));
    } else {
        esp_err_t wifi_power_result = wifi_manager_set_power_save(
            settings->wifi_power_save_enabled);
        if (wifi_power_result != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi 节能策略未生效: %s", esp_err_to_name(wifi_power_result));
        }
    }

    esp_err_t passport_result = kiro_passport_network_init();
    s_ok[3] = (passport_result == ESP_OK);
    if (passport_result != ESP_OK) {
        ESP_LOGW(TAG, "Kiro Passport Wi-Fi 初始化失败: %s", esp_err_to_name(passport_result));
    }

    screencast_init();

    if (bsp_lvgl_lock(1000)) {
        ui_status_init();
        ui_status_set_time(settings->hour, settings->minute, settings->second);
        ui_status_set_time_format(settings->time_format == APP_SETTINGS_TIME_HH_MM_SS
                                      ? UI_STATUS_TIME_HH_MM_SS : UI_STATUS_TIME_HH_MM);
        enter_menu();
        bsp_lvgl_unlock();
    }

    bsp_display_backlight(app_settings_get_brightness_percent());

    ESP_LOGI(TAG, "就绪:按键=%d 音频=%d 电量=%d 阅读=%d 图片=%d 游戏=%d Kiro=%d Chat=%d 设置=%d Wi-Fi=%d",
             button_ok, audio_ok, batt_ok, s_ok[0], s_ok[1], s_ok[2], s_ok[3], s_ok[4], s_ok[5],
             wifi_result == ESP_OK);
}
