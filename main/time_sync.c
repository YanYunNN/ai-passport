#include "time_sync.h"
#include "wifi_manager.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <sys/time.h>

#define TIME_SYNC_TIMEOUT_MS 15000
#define TIME_SYNC_POLL_MS 250
#define TIME_SYNC_SERVER "pool.ntp.org"
/* Default to China Standard Time (UTC+8); make this product-configurable if needed. */
#define TIME_SYNC_TIMEZONE "CST-8"

static time_sync_state_t s_state = TIME_SYNC_IDLE;
static time_t s_epoch;
static time_t s_callback_epoch;
static bool s_epoch_available;
static bool s_sync_callback_received;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void set_state(time_sync_state_t state)
{
    taskENTER_CRITICAL(&s_lock);
    s_state = state;
    taskEXIT_CRITICAL(&s_lock);
}

static void time_sync_notification(struct timeval *time_value)
{
    if (!time_value) return;

    taskENTER_CRITICAL(&s_lock);
    s_callback_epoch = time_value->tv_sec;
    s_sync_callback_received = true;
    taskEXIT_CRITICAL(&s_lock);
}

static bool take_synchronized_epoch(time_t *epoch)
{
    bool received;

    taskENTER_CRITICAL(&s_lock);
    received = s_sync_callback_received;
    if (received) s_sync_callback_received = false;
    if (received) *epoch = s_callback_epoch;
    taskEXIT_CRITICAL(&s_lock);
    return received;
}

static void time_sync_task(void *argument)
{
    (void)argument;
    setenv("TZ", TIME_SYNC_TIMEZONE, 1);
    tzset();

    if (esp_sntp_enabled()) esp_sntp_stop();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    esp_sntp_setservername(2, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification);
    esp_sntp_init();

    TickType_t started = xTaskGetTickCount();
    while ((xTaskGetTickCount() - started) < pdMS_TO_TICKS(TIME_SYNC_TIMEOUT_MS)) {
        if (wifi_manager_get_state() != WIFI_MANAGER_CONNECTED) {
            esp_sntp_stop();
            set_state(TIME_SYNC_NO_WIFI);
            vTaskDelete(NULL);
            return;
        }

        time_t synchronized_epoch;
        if (take_synchronized_epoch(&synchronized_epoch)) {
            esp_sntp_stop();
            taskENTER_CRITICAL(&s_lock);
            s_epoch = synchronized_epoch;
            s_epoch_available = true;
            s_state = TIME_SYNC_SUCCESS;
            taskEXIT_CRITICAL(&s_lock);
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_POLL_MS));
    }

    esp_sntp_stop();
    set_state(TIME_SYNC_TIMEOUT);
    vTaskDelete(NULL);
}

esp_err_t time_sync_request(void)
{
    if (wifi_manager_get_state() != WIFI_MANAGER_CONNECTED) {
        set_state(TIME_SYNC_NO_WIFI);
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_lock);
    if (s_state == TIME_SYNC_SYNCING) {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_state = TIME_SYNC_SYNCING;
    s_epoch_available = false;
    s_sync_callback_received = false;
    taskEXIT_CRITICAL(&s_lock);

    if (xTaskCreate(time_sync_task, "time_sync", 4096, NULL, 4, NULL) == pdPASS) {
        return ESP_OK;
    }

    set_state(TIME_SYNC_FAILED);
    return ESP_ERR_NO_MEM;
}

time_sync_state_t time_sync_get_state(void)
{
    taskENTER_CRITICAL(&s_lock);
    time_sync_state_t state = s_state;
    taskEXIT_CRITICAL(&s_lock);
    return state;
}

bool time_sync_take_epoch(time_t *epoch)
{
    if (!epoch) return false;

    taskENTER_CRITICAL(&s_lock);
    bool available = s_epoch_available;
    if (available) {
        *epoch = s_epoch;
        s_epoch_available = false;
    }
    taskEXIT_CRITICAL(&s_lock);
    return available;
}
