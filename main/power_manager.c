#include "power_manager.h"
#include "app_settings.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "bsp_button.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power_manager";
static bool s_initialized;
static bool s_light_sleep_enabled;
static volatile uint32_t s_last_activity_time_sec;
static volatile bool s_screen_dimmed;
static esp_timer_handle_t s_inactivity_timer;

static void inactivity_timer_cb(void *arg)
{
    (void)arg;
    uint16_t screen_timeout = app_settings_get_screen_timeout_seconds();
    uint16_t auto_sleep_timeout = app_settings_get_auto_sleep_timeout_seconds();
    if (screen_timeout == 0 && auto_sleep_timeout == 0) return;

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    uint32_t elapsed = (now >= s_last_activity_time_sec) ? (now - s_last_activity_time_sec) : 0;

    if (auto_sleep_timeout > 0 && elapsed >= auto_sleep_timeout) {
        ESP_LOGI(TAG, "空闲超时 %u 秒，自动进入深度休眠", (unsigned)elapsed);
        power_manager_enter_deep_sleep();
        return;
    }

    if (screen_timeout > 0 && elapsed >= screen_timeout && !s_screen_dimmed) {
        s_screen_dimmed = true;
        bsp_display_backlight(0);
        ESP_LOGI(TAG, "空闲超时 %u 秒，自动息屏", (unsigned)elapsed);
    }
}

static void log_wakeup_cause(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause) {
    case ESP_SLEEP_WAKEUP_GPIO:
        ESP_LOGW(TAG, "上次深睡由 GPIO 低电平唤醒 (mask=0x%llx)", (unsigned long long)esp_sleep_get_gpio_wakeup_status());
        break;
    case ESP_SLEEP_WAKEUP_UNDEFINED:
        // 上电复位或软件复位,不是从深睡唤醒
        break;
    default:
        ESP_LOGI(TAG, "上次深睡唤醒原因: %d", (int)cause);
        break;
    }
}

static esp_err_t apply_power_policy(bool light_sleep_enabled)
{
#if CONFIG_PM_ENABLE
    const esp_pm_config_t config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 10,
        .light_sleep_enable = light_sleep_enabled,
    };
    esp_err_t result = esp_pm_configure(&config);
    if (result == ESP_OK) {
        s_light_sleep_enabled = light_sleep_enabled;
        ESP_LOGI(TAG, "自动浅睡眠%s", light_sleep_enabled ? "已启用" : "已关闭");
    } else {
        ESP_LOGW(TAG, "配置电源管理失败: %s", esp_err_to_name(result));
    }
    return result;
#else
    (void)light_sleep_enabled;
    ESP_LOGW(TAG, "固件未启用 CONFIG_PM_ENABLE，无法配置浅睡眠");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t power_manager_init(bool light_sleep_enabled)
{
    log_wakeup_cause();
    s_last_activity_time_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    s_screen_dimmed = false;

    if (!s_inactivity_timer) {
        const esp_timer_create_args_t timer_args = {
            .callback = inactivity_timer_cb,
            .name = "inactivity_check",
        };
        esp_err_t timer_res = esp_timer_create(&timer_args, &s_inactivity_timer);
        if (timer_res == ESP_OK) {
            esp_timer_start_periodic(s_inactivity_timer, 1000000); // 1s
        }
    }

    if (s_initialized) return power_manager_set_light_sleep_enabled(light_sleep_enabled);

    esp_err_t result = apply_power_policy(light_sleep_enabled);
    if (result == ESP_OK) s_initialized = true;
    return result;
}

esp_err_t power_manager_set_light_sleep_enabled(bool enabled)
{
    return apply_power_policy(enabled);
}

bool power_manager_is_light_sleep_enabled(void)
{
    return s_light_sleep_enabled;
}

bool power_manager_activity_notify(void)
{
    s_last_activity_time_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    if (s_screen_dimmed) {
        s_screen_dimmed = false;
        bsp_display_backlight(app_settings_get_brightness_percent());
        ESP_LOGI(TAG, "检测到按键，恢复屏幕亮度");
        return true;
    }
    return false;
}

bool power_manager_is_screen_dimmed(void)
{
    return s_screen_dimmed;
}

void power_manager_wake_screen(void)
{
    s_last_activity_time_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    s_screen_dimmed = false;
    bsp_display_backlight(app_settings_get_brightness_percent());
}

// 深睡唤醒配置的是 GPIO 低电平触发:若触发本次休眠的按键在入眠瞬间仍被按住,
// GPIO 在进入深睡时仍为低电平,芯片会在睡下去的一瞬间被自己唤醒,表现为
// "屏幕闪一下又回到菜单,根本没睡"。因此入眠前必须等按键完全松开。
// 松开判定用 ADC 电压:按住时 ≤ 确定键上界 1900mV,松开后被外部 10k 上拉抬到 3300mV。
#define BTN_RELEASE_MV_THRESHOLD 1900

static void wait_for_button_release(void)
{
    const uint32_t timeout_ms = 3000;
    const uint32_t step_ms = 10;
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        int mv = bsp_button_read_mv();
        bool pressed = (mv >= 0 && mv <= BTN_RELEASE_MV_THRESHOLD);
        if (!pressed) {
            // 再确认 50ms,避免把按下/松开的临界抖动误判为已松开
            vTaskDelay(pdMS_TO_TICKS(50));
            mv = bsp_button_read_mv();
            if (mv < 0 || mv > BTN_RELEASE_MV_THRESHOLD) return;
        }
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        waited += step_ms;
    }
    ESP_LOGW(TAG, "等待按键松开超时(%u ms),仍按原计划进入深睡", (unsigned)timeout_ms);
}

void power_manager_enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "正在进入深度休眠 (Deep Sleep)...");
    bsp_display_backlight(0);
    wait_for_button_release();
    esp_wifi_stop();
    esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
}
