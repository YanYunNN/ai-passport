#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <time.h>

typedef enum {
    TIME_SYNC_IDLE,
    TIME_SYNC_SYNCING,
    TIME_SYNC_SUCCESS,
    TIME_SYNC_NO_WIFI,
    TIME_SYNC_TIMEOUT,
    TIME_SYNC_FAILED,
} time_sync_state_t;

/* Starts one asynchronous NTP request. Requires WIFI_MANAGER_CONNECTED. */
esp_err_t time_sync_request(void);
time_sync_state_t time_sync_get_state(void);

/* Returns a newly synchronized local-clock epoch once. Safe to call from the UI task. */
bool time_sync_take_epoch(time_t *epoch);
