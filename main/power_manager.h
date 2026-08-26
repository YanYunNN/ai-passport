#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* Configures dynamic frequency scaling, automatic light sleep, and inactivity monitoring. */
esp_err_t power_manager_init(bool light_sleep_enabled);
esp_err_t power_manager_set_light_sleep_enabled(bool enabled);
bool power_manager_is_light_sleep_enabled(void);

/* Activity notification from user input (resets inactivity timer).
 * Returns true if the screen was dimmed and has now been woken up. */
bool power_manager_activity_notify(void);

/* Check if the display backlight is currently dimmed/off due to inactivity timeout. */
bool power_manager_is_screen_dimmed(void);

/* Explicitly restore screen brightness to the configured level. */
void power_manager_wake_screen(void);

/* Puts the device into ultra-low-power deep sleep (~5uA), wakeable via any button (GPIO0). */
void power_manager_enter_deep_sleep(void);
