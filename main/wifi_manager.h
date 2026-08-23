#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    WIFI_MANAGER_UNAVAILABLE,
    WIFI_MANAGER_UNCONFIGURED,
    WIFI_MANAGER_PROVISIONING,
    WIFI_MANAGER_CONNECTING,
    WIFI_MANAGER_CONNECTED,
    WIFI_MANAGER_FAILED,
} wifi_manager_state_t;

/* 初始化 NVS、网络栈与 STA；已有成功配置时自动尝试重连。 */
esp_err_t wifi_manager_init(void);
wifi_manager_state_t wifi_manager_get_state(void);

/* 启动受 WPA2 密码保护的临时 SoftAP 配置页。 */
esp_err_t wifi_manager_start_provisioning(void);
void wifi_manager_stop_provisioning(void);
const char *wifi_manager_get_provisioning_ssid(void);
const char *wifi_manager_get_provisioning_password(void);
