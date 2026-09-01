// components/bsp/include/bsp_battery.h
// CellWise CW2017 电量计:I2C 0x63,与 ES8311 共用总线。
// 芯片自带 Li-Poly profile,直接给 SOC%,无需外部分压电阻与查表。
#pragma once

#include "esp_err.h"

// 初始化。内部会调 bsp_i2c_init()(幂等)，并按 CW2017 官方唤醒序列
// (MODE: 0x30 -> 0x00)把芯片带到正常态，最多重试 3 次并等待首个有效 SOC
// 出现(完全放电后充电开机的板子可能需要多次唤醒)。
// 芯片不应答时返回 ESP_ERR_NOT_FOUND；芯片在位但始终未就绪时不视为失败，
// 返回 ESP_OK，由后续 bsp_battery_soc() 读取时自动限频重试唤醒。
esp_err_t bsp_battery_init(void);

// 剩余电量百分比 0..100，直接读电量计 0x04/0x05(高字节=整数百分比，
// 低字节=1/256 %)并四舍五入。芯片未就绪(SOC=0xFF)时会自动执行唤醒序列
// 后短轮询，仍无效返回 -1。
int bsp_battery_soc(void);

// 电池电压 mV;读失败返回 -1。
int bsp_battery_mv(void);
