#pragma once

#include "esp_err.h"
#include <stdbool.h>

/* Configures dynamic frequency scaling and optional automatic light sleep. */
esp_err_t power_manager_init(bool light_sleep_enabled);
esp_err_t power_manager_set_light_sleep_enabled(bool enabled);
bool power_manager_is_light_sleep_enabled(void);
