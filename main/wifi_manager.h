#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    WIFI_MANAGER_UNAVAILABLE,
    WIFI_MANAGER_DISABLED,
    WIFI_MANAGER_UNCONFIGURED,
    WIFI_MANAGER_PROVISIONING,
    WIFI_MANAGER_CONNECTING,
    WIFI_MANAGER_CONNECTED,
    WIFI_MANAGER_FAILED,
} wifi_manager_state_t;

/* Sets the desired radio state. It may be called before init to select boot behavior. */
esp_err_t wifi_manager_set_enabled(bool enabled);
bool wifi_manager_is_enabled(void);

/* Requests modem sleep for STA operation. The selection is retained while the radio is off. */
esp_err_t wifi_manager_set_power_save(bool enabled);
bool wifi_manager_is_power_save_enabled(void);

/* Initializes NVS, the network stack and STA; saved credentials reconnect when enabled. */
esp_err_t wifi_manager_init(void);
wifi_manager_state_t wifi_manager_get_state(void);

/* Only returns the associated SSID while connected; never exposes the password. */
esp_err_t wifi_manager_get_connected_ssid(char *ssid, size_t size);

/* Starts a WPA2-protected temporary SoftAP configuration page while Wi-Fi is enabled. */
esp_err_t wifi_manager_start_provisioning(void);
void wifi_manager_stop_provisioning(void);
const char *wifi_manager_get_provisioning_ssid(void);
const char *wifi_manager_get_provisioning_password(void);
