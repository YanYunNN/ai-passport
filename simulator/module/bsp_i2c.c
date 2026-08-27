/* simulator/module/bsp_i2c.c
 * 虚拟 I2C（P0 桩）：总线上无设备。
 */
#include "bsp_i2c.h"

#include <stdio.h>

esp_err_t bsp_i2c_init(void)
{
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_bus(void)
{
    return NULL;
}

esp_err_t bsp_i2c_scan(void)
{
    printf("[sim] i2c scan: no bus simulated (P0)\n");
    return ESP_OK;
}
