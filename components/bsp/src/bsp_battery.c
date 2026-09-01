// components/bsp/src/bsp_battery.c
// 移植自 trae_card/components/platform/platform_esp32/src/battery_cw2017.c
// (去掉了电池 profile 写入部分:开源硬件用户电池各异,用芯片自带 Li-Poly profile 更通用)
#include "bsp_battery.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdbool.h>

static const char *TAG = "bsp_batt";

#define CW_REG_VERSION   0x00   // 版本号,上电应答即代表芯片在位
#define CW_REG_VCELL_H   0x02   // 14bit 电压,V(uV) = raw * 312.5
#define CW_REG_SOC_H     0x04   // 高字节 = 整数百分比;低字节(0x05)= 1/256 %
#define CW_REG_MODE      0x08   // 0xF0=睡眠 / 0x30=复位态 / 0x00=正常

#define CW_MODE_RESTART  0x30
#define CW_MODE_NORMAL   0x00

// 唤醒(快速启动)后芯片要跑完若干采样周期才会给出首个有效 SOC(未就绪时读回 0xFF)。
// init 里每轮最多轮询 CW_SOC_POLLS 次(每 50ms 一次),最多重试 CW_READY_ATTEMPTS 轮;
// 完全放电后刚接上充电器时芯片可能要多唤醒几次才就绪。
#define CW_SOC_POLLS      10u    // 单轮最多 500ms
#define CW_READY_ATTEMPTS 3u     // init 最多唤醒尝试次数
#define CW_SOC_RETRY_MS   5000u  // soc() 里两次自动唤醒的最小间隔(避免反复重启打断采样)

static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_lock;        // 保护 s_dev 与唤醒状态,init/soc/mv 共用
static uint32_t s_last_quickstart;      // tick;与 CW_SOC_RETRY_MS 一起限频

static int cw_read(uint8_t reg, uint8_t *buf, size_t n) {
    if (!s_dev) return -1;
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100) == ESP_OK ? 0 : -1;
}

static int cw_write(uint8_t reg, uint8_t val) {
    if (!s_dev) return -1;
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, 100) == ESP_OK ? 0 : -1;
}

// 取锁;返回 false 表示锁不可用(内存不足或超时)。
static bool battery_lock(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return false;
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE;
}

// 官方唤醒/快速启动序列:写 0x30 触发重启,等芯片完成复位后再写 0x00 回到正常态。
// 单独写 0x00 无法可靠唤醒经历过掉电(完全放电后充电)的电量计。
static void cw_quickstart(void) {
    if (cw_write(CW_REG_MODE, CW_MODE_RESTART) != 0) {
        ESP_LOGW(TAG, "CW2017 写入 RESTART 失败");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    if (cw_write(CW_REG_MODE, CW_MODE_NORMAL) != 0) {
        ESP_LOGE(TAG, "CW2017 写回 NORMAL 失败");
        return;
    }
    s_last_quickstart = xTaskGetTickCount();
}

// 就绪判定:处于正常态(MODE=0x00)且 SOC 高字节 <= 100。
// SOC 高字节 0xFF 表示芯片尚未完成首次采样,读到的百分比无效。
static bool cw_ready(void) {
    uint8_t mode = 0, soc[2] = {0xFF, 0xFF};
    if (cw_read(CW_REG_MODE, &mode, 1) != 0) return false;
    if (mode != CW_MODE_NORMAL) return false;
    if (cw_read(CW_REG_SOC_H, soc, 2) != 0) return false;
    return soc[0] <= 100;
}

// SOC 16bit 读数(高字节=整数百分比,低字节=1/256 %)四舍五入到整数百分比。
static int cw_soc_pct(const uint8_t b[2]) {
    int pct = (int)((((uint32_t)b[0] << 8) | b[1]) + 128u) >> 8;
    return pct > 100 ? 100 : pct;
}

// 已持有锁:确保 I2C 设备存在且芯片在位。不在位时清理句柄并返回 ESP_ERR_NOT_FOUND。
static esp_err_t battery_ensure_device(void) {
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
    return ESP_OK;
}

esp_err_t bsp_battery_init(void) {
    if (!battery_lock()) return ESP_ERR_NO_MEM;

    esp_err_t e = battery_ensure_device();
    if (e == ESP_OK) {
        // 完全放电后刚接上充电器时,芯片可能处于睡眠态或 SOC 长时间未就绪(0xFF),
        // 只做一次 0x30 -> 0x00 不一定够;这里最多重试 CW_READY_ATTEMPTS 轮。
        for (uint32_t attempt = 0; attempt < CW_READY_ATTEMPTS && !cw_ready(); attempt++) {
            ESP_LOGI(TAG, "CW2017 未就绪, 执行唤醒 (第 %u/%u 次)", attempt + 1u, CW_READY_ATTEMPTS);
            cw_quickstart();
            for (uint32_t i = 0; i < CW_SOC_POLLS && !cw_ready(); i++) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
    }
    bool ready = (e == ESP_OK) && cw_ready();
    xSemaphoreGive(s_lock);

    if (ready) {
        ESP_LOGI(TAG, "CW2017 就绪, 当前 SOC=%d%%, 电压=%dmV", bsp_battery_soc(), bsp_battery_mv());
    } else if (e == ESP_OK) {
        // 芯片在位但始终未就绪:不当作失败,后续 bsp_battery_soc() 会限频自动唤醒。
        ESP_LOGW(TAG, "CW2017 仍处于未就绪态, 后续读取会自动重试唤醒");
    }
    return e;
}

int bsp_battery_soc(void) {
    if (!battery_lock()) return -1;

    int soc = -1;
    bool responded = false;
    uint8_t b[2] = {0xFF, 0xFF};

    if (battery_ensure_device() == ESP_OK) {
        if (cw_read(CW_REG_SOC_H, b, 2) == 0) {
            responded = true;
            if (b[0] <= 100) soc = cw_soc_pct(b);
        }

        // 芯片在位但 SOC 未就绪(高字节 0xFF,多见于完全放电后充电开机):
        // 限频重试唤醒序列并短轮询,让这类场景自动恢复,而不是一直返回无效值。
        uint32_t now = xTaskGetTickCount();
        if (soc < 0 && responded && b[0] > 100 &&
            (s_last_quickstart == 0 || now - s_last_quickstart >= CW_SOC_RETRY_MS)) {
            cw_quickstart();
            for (uint32_t i = 0; i < CW_SOC_POLLS / 2u && soc < 0; i++) {
                vTaskDelay(pdMS_TO_TICKS(50));
                if (cw_read(CW_REG_SOC_H, b, 2) == 0 && b[0] <= 100) {
                    soc = cw_soc_pct(b);
                }
            }
        }
    }
    xSemaphoreGive(s_lock);
    return soc;
}

int bsp_battery_mv(void) {
    if (!battery_lock()) return -1;

    int mv = -1;
    if (battery_ensure_device() == ESP_OK) {
        uint8_t b[2] = { 0 };
        if (cw_read(CW_REG_VCELL_H, b, 2) == 0) {
            uint32_t raw = ((uint32_t)b[0] << 8 | b[1]) & 0x3FFF;   // 14bit
            mv = (int)((raw * 3125) / 10000);                       // raw * 312.5uV → mV
        }
    }
    xSemaphoreGive(s_lock);
    return mv;
}
