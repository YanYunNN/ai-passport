#include "power_manager.h"
#include "esp_log.h"
#include "esp_pm.h"

static const char *TAG = "power_manager";
static bool s_initialized;
static bool s_light_sleep_enabled;

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
