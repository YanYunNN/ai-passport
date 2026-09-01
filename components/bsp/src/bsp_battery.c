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
#define CW_REG_MODE      0x08   // 0xF0/0xC0=睡眠 / 0x30=复位态 / 0x00=正常

#define CW_MODE_RESTART  0x30
#define CW_MODE_NORMAL   0x00

static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_lock;

static int cw_read(uint8_t reg, uint8_t *buf, size_t n) {
    if (!s_dev) return -1;
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100) == ESP_OK ? 0 : -1;
}

static int cw_write(uint8_t reg, uint8_t val) {
    if (!s_dev) return -1;
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, 100) == ESP_OK ? 0 : -1;
}

static bool battery_lock(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return false;
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE;
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
        // 唤醒/启动芯片:
        // 写 0x30 触发 QuickStart 重启算法, 延时后写 0x00 回到正常工作态。
        // 只在初始化时执行一次, 绝不在后续读取中反复打断。
        cw_write(CW_REG_MODE, CW_MODE_RESTART);
        vTaskDelay(pdMS_TO_TICKS(40));
        cw_write(CW_REG_MODE, CW_MODE_NORMAL);
        vTaskDelay(pdMS_TO_TICKS(100));

        // 轮询等待首次采样就绪 (最多等待 1 秒)
        for (int i = 0; i < 20; i++) {
            uint8_t b[2] = {0xFF, 0xFF};
            if (cw_read(CW_REG_SOC_H, b, 2) == 0 && b[0] <= 100) {
                ESP_LOGI(TAG, "CW2017 采样就绪, SOC=%d%%", cw_soc_pct(b));
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    xSemaphoreGive(s_lock);
    return e;
}

// 典型单节 3.7V/4.2V 锂聚合物电池开路电压 (OCV) 映射表
static int ocv_to_soc(int mv) {
    if (mv >= 4200) return 100;
    if (mv <= 3350) return 0;

    static const struct { int mv; int soc; } OCV_MAP[] = {
        { 4200, 100 },
        { 4120, 92 },
        { 4020, 80 },
        { 3920, 66 },
        { 3830, 50 },
        { 3760, 35 },
        { 3700, 20 },
        { 3640, 10 },
        { 3550, 4 },
        { 3350, 0 },
    };
    for (size_t i = 0; i < (sizeof(OCV_MAP) / sizeof(OCV_MAP[0])) - 1; i++) {
        if (mv >= OCV_MAP[i + 1].mv) {
            int v_high = OCV_MAP[i].mv;
            int v_low  = OCV_MAP[i + 1].mv;
            int s_high = OCV_MAP[i].soc;
            int s_low  = OCV_MAP[i + 1].soc;
            return s_low + (mv - v_low) * (s_high - s_low) / (v_high - v_low);
        }
    }
    return 0;
}

int bsp_battery_mv(void) {
    if (!battery_lock()) return -1;

    int mv = -1;
    if (battery_ensure_device() == ESP_OK) {
        uint8_t b[2] = { 0 };
        if (cw_read(CW_REG_VCELL_H, b, 2) == 0) {
            uint32_t raw = ((uint32_t)b[0] << 8 | b[1]) & 0x3FFF;   // 14bit
            int val = (int)((raw * 3125) / 10000);                  // raw * 312.5uV → mV
            if (val >= 2500 && val <= 4600) {                       // 合理单节锂电范围
                mv = val;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return mv;
}

int bsp_battery_soc(void) {
    if (!battery_lock()) return -1;

    int soc = -1;
    if (battery_ensure_device() == ESP_OK) {
        uint8_t b[2] = {0xFF, 0xFF};
        int hw_soc = -1;
        if (cw_read(CW_REG_SOC_H, b, 2) == 0 && b[0] <= 100) {
            hw_soc = cw_soc_pct(b);
        }

        // 读取当前真实电压 (mV)
        uint8_t vb[2] = {0};
        int mv = -1;
        if (cw_read(CW_REG_VCELL_H, vb, 2) == 0) {
            uint32_t raw = ((uint32_t)vb[0] << 8 | vb[1]) & 0x3FFF;
            int val = (int)((raw * 3125) / 10000);
            if (val >= 2500 && val <= 4600) mv = val;
        }

        if (mv > 0) {
            // 如果硬件 FastCali 已给出非零有效 SOC，且与电压大致吻合，使用芯片计算值；
            // 若芯片未烧录 Profile 导致读数为 0% 或未就绪，则回退到标准 OCV 曲线计算。
            if (hw_soc > 0 && !(hw_soc == 0 && mv >= 3500)) {
                soc = hw_soc;
            } else {
                soc = ocv_to_soc(mv);
            }
        }
    }
    xSemaphoreGive(s_lock);
    return soc;
}

