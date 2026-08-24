// components/bsp/include/bsp_battery.h
// CellWise CW2017 电量计:I2C 0x63,与 ES8311 共用总线。
// 芯片自带 Li-Poly profile,直接给 SOC%,无需外部分压电阻与查表。
#pragma once

#include "esp_err.h"

// 初始化。内部会调 bsp_i2c_init()(幂等)，并按 CW2017 官方序列将
// 上电默认睡眠态从 MODE=0x30 切换至 MODE=0x00 的正常工作态。
// 芯片不应答时返回 ESP_ERR_NOT_FOUND；模式切换失败时返回 ESP_FAIL。
esp_err_t bsp_battery_init(void);

// 剩余电量百分比 0..100;读失败返回 -1。
int bsp_battery_soc(void);

// 电池电压 mV;读失败返回 -1。
int bsp_battery_mv(void);
