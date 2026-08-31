// components/bsp/src/bsp_battery.c
// 移植自 trae_card/components/platform/platform_esp32/src/battery_cw2017.c
// (去掉了电池 profile 写入部分:开源硬件用户电池各异,用芯片自带 Li-Poly profile 更通用)
#include "bsp_battery.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_batt";

#define CW_REG_VERSION   0x00   // 版本号,上电应答即代表芯片在位
#define CW_REG_VCELL_H   0x02   // 14bit 电压,V(uV) = raw * 312.5
#define CW_REG_SOC_H     0x04   // 高字节 = 整数百分比;低字节(0x05)= 1/256 %
#define CW_REG_MODE      0x08   // 0xF0=睡眠 / 0x30=复位态 / 0x00=正常

#define CW_MODE_RESTART  0x30
#define CW_MODE_NORMAL   0x00

static i2c_master_dev_handle_t s_dev;

static int cw_read(uint8_t reg, uint8_t *buf, size_t n) {
    if (!s_dev) return -1;
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100) == ESP_OK ? 0 : -1;
}

static int cw_write(uint8_t reg, uint8_t val) {
    if (!s_dev) return -1;
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, 100) == ESP_OK ? 0 : -1;
}

esp_err_t bsp_battery_init(void) {
    if (s_dev) return ESP_OK;

    esp_err_t e = bsp_i2c_init();
    if (e != ESP_OK) return e;

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BSP_I2C_CW2017_ADDR,
        .scl_speed_hz    = 100000,
    };
    e = i2c_master_bus_add_device(bsp_i2c_bus(), &dc, &s_dev);
    if (e != ESP_OK) { ESP_LOGE(TAG, "添加 I2C 设备失败: %s", esp_err_to_name(e)); return e; }

    uint8_t ver = 0;
    if (cw_read(CW_REG_VERSION, &ver, 1) != 0) {
        ESP_LOGW(TAG, "CW2017 未应答 —— 用 bsp_i2c_scan() 确认 0x%02X 是否在线;"
                      "无电量计的板子可忽略本项", BSP_I2C_CW2017_ADDR);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "检测到 CW2017 VERSION=0x%02X", ver);

    uint8_t mode = 0;
    cw_read(CW_REG_MODE, &mode, 1);
    ESP_LOGI(TAG, "CW2017 当前 MODE=0x%02X", mode);

    // 若不在正常工作态 (0x00) 或 SOC 处于未就绪态 (0xFF)，执行官方唤醒时序
    uint8_t soc_test[2] = {0xFF, 0xFF};
    cw_read(CW_REG_SOC_H, soc_test, 2);

    if (mode != CW_MODE_NORMAL || soc_test[0] > 100) {
        ESP_LOGI(TAG, "CW2017 处于休眠或未就绪态 (mode=0x%02X, soc_raw=0x%02X), 执行唤醒...", mode, soc_test[0]);
        if (cw_write(CW_REG_MODE, CW_MODE_RESTART) != 0) {
            ESP_LOGW(TAG, "CW2017 写入 RESTART 警告");
        }
        vTaskDelay(pdMS_TO_TICKS(30));
        if (cw_write(CW_REG_MODE, CW_MODE_NORMAL) != 0) {
            ESP_LOGE(TAG, "CW2017 模式唤醒失败");
            i2c_master_bus_rm_device(s_dev);
            s_dev = NULL;
            return ESP_FAIL;
        }
    }

    // 等待首次采样完成(最多等 500ms,每 50ms 检查一次)
    for (int i = 0; i < 10; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
        uint8_t soc_raw[2] = {0xFF, 0xFF};
        if (cw_read(CW_REG_SOC_H, soc_raw, 2) == 0 && soc_raw[0] <= 100) {
            break;
        }
    }
    ESP_LOGI(TAG, "CW2017 就绪, 当前 SOC=%d%%, 电压=%dmV", bsp_battery_soc(), bsp_battery_mv());
    return ESP_OK;
}

int bsp_battery_soc(void) {
    if (!s_dev) {
        if (bsp_battery_init() != ESP_OK) return -1;
    }
    uint8_t b[2] = { 0 };
    if (cw_read(CW_REG_SOC_H, b, 2) != 0) return -1;
    int soc = b[0];                       // 高字节即整数百分比
    if (soc > 100) return -1;             // 芯片未就绪时可能读到 0xFF
    return soc;
}

int bsp_battery_mv(void) {
    if (!s_dev) {
        if (bsp_battery_init() != ESP_OK) return -1;
    }
    uint8_t b[2] = { 0 };
    if (cw_read(CW_REG_VCELL_H, b, 2) != 0) return -1;
    uint32_t raw = ((uint32_t)b[0] << 8 | b[1]) & 0x3FFF;   // 14bit
    return (int)((raw * 3125) / 10000);                     // raw * 312.5uV → mV
}
