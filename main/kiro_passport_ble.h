#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool initialized;
    bool advertising;
    bool connected;
    bool encrypted;
    bool subscribed;
    bool pending;
    char state[12];
    char request_id[37];
    char tool[32];
    char summary[72];
    char decision[8];
} kiro_passport_snapshot_t;

// 初始化一次 NimBLE peripheral。此服务的生命周期独立于 Kiro 页面。
esp_err_t kiro_passport_ble_init(void);

// 读取线程安全快照；可由 LVGL 定时器或按键回调使用。
void kiro_passport_ble_get_snapshot(kiro_passport_snapshot_t *snapshot);

// 对当前请求作出设备侧决定。没有待处理请求时返回 ESP_ERR_INVALID_STATE。
esp_err_t kiro_passport_ble_decide(bool allow);
