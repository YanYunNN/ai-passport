/* simulator/module/bsp_battery.c
 * 虚拟电量计（P0 桩）：返回可配置的模拟值。
 * 默认 87% / 3900mV；测试低电量 UI 时可调用 bsp_battery_sim_set()。
 */
#include "bsp_battery.h"
#include "module_internal.h"

static int s_soc = 87;
static int s_mv = 3900;

esp_err_t bsp_battery_init(void)
{
    return ESP_OK;
}

int bsp_battery_soc(void)
{
    return s_soc;
}

int bsp_battery_mv(void)
{
    return s_mv;
}

void bsp_battery_sim_set(int soc, int mv)
{
    if (soc < 0) soc = 0;
    if (soc > 100) soc = 100;
    s_soc = soc;
    s_mv = mv;
}
