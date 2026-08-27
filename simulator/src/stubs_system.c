/* simulator/src/stubs_system.c
 * 电源管理 / 投屏 / 时间同步 的宿主桩（P0 无真实行为）。
 * 保持 main/ 源码原样可链接：返回合理的空闲状态，不做任何硬件动作。
 */
#include "power_manager.h"
#include "screencast.h"
#include "time_sync.h"

#include <stdio.h>

/* ==================== power_manager ==================== */
static bool s_light_sleep_enabled;
static bool s_screen_dimmed;

esp_err_t power_manager_init(bool light_sleep_enabled)
{
    s_light_sleep_enabled = light_sleep_enabled;
    s_screen_dimmed = false;
    return ESP_OK;
}

esp_err_t power_manager_set_light_sleep_enabled(bool enabled)
{
    s_light_sleep_enabled = enabled;
    return ESP_OK;
}

bool power_manager_is_light_sleep_enabled(void)
{
    return s_light_sleep_enabled;
}

bool power_manager_activity_notify(void)
{
    bool was_dimmed = s_screen_dimmed;
    s_screen_dimmed = false;
    return was_dimmed;
}

bool power_manager_is_screen_dimmed(void)
{
    return s_screen_dimmed;
}

void power_manager_wake_screen(void)
{
    s_screen_dimmed = false;
}

void power_manager_enter_deep_sleep(void)
{
    printf("[sim] deep sleep requested (P0 无低功耗语义，忽略)\n");
}

/* ==================== screencast ==================== */
static bool s_screencast_enabled;

esp_err_t screencast_init(void)
{
    return ESP_OK;
}

void screencast_request_capture(void)
{
    /* P0：无网络上报 */
}

void screencast_set_enabled(bool enabled)
{
    s_screencast_enabled = enabled;
}

bool screencast_is_enabled(void)
{
    return s_screencast_enabled;
}

/* ==================== time_sync ==================== */
esp_err_t time_sync_request(void)
{
    return ESP_ERR_NOT_SUPPORTED; /* P0 无网络 */
}

time_sync_state_t time_sync_get_state(void)
{
    return TIME_SYNC_IDLE;
}

bool time_sync_take_epoch(time_t *epoch)
{
    (void)epoch;
    return false;
}
