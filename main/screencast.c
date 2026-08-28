#include "screencast.h"
#include "bsp_display.h"
#include "kiro_passport_network.h"
#include "lvgl.h"
#include "display/lv_display_private.h"
#include "core/lv_refr_private.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "screencast";

#define SCREENCAST_WIDTH         240
#define SCREENCAST_HEIGHT        320
#define SCREENCAST_SLICE_LINES   4
#define SCREENCAST_TOTAL_SLICES  (SCREENCAST_HEIGHT / SCREENCAST_SLICE_LINES) // 80
#define SCREENCAST_HEADER_SIZE   16
#define SCREENCAST_RAW_SIZE      (SCREENCAST_WIDTH * SCREENCAST_SLICE_LINES * 2) // 1920 bytes
#define SCREENCAST_PACKET_SIZE   (SCREENCAST_HEADER_SIZE + SCREENCAST_RAW_SIZE) // 1936 bytes
#define SCREENCAST_ACK_TIMEOUT_MS 3000

_Static_assert(SCREENCAST_HEIGHT % SCREENCAST_SLICE_LINES == 0,
               "screencast slices must cover the display exactly");
_Static_assert(SCREENCAST_PACKET_SIZE <= 2048,
               "screencast packet must fit the WebSocket buffer");

static SemaphoreHandle_t s_trigger_sem = NULL;
static SemaphoreHandle_t s_ack_sem = NULL;
static TaskHandle_t s_task_handle = NULL;
static uint32_t s_frame_seq = 0;
static volatile uint32_t s_expected_ack_seq = UINT32_MAX;
static volatile uint8_t s_expected_ack_slice = UINT8_MAX;

static uint8_t s_packet_buf[SCREENCAST_PACKET_SIZE];

static void write_u16_be(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static void write_u32_be(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static void prepare_packet_header(uint32_t frame_seq, uint8_t slice)
{
    s_packet_buf[0] = 'S';
    s_packet_buf[1] = 'C';
    s_packet_buf[2] = 1;
    s_packet_buf[3] = SCREENCAST_SLICE_LINES;
    write_u32_be(s_packet_buf + 4, frame_seq);
    write_u16_be(s_packet_buf + 8, slice);
    write_u16_be(s_packet_buf + 10, SCREENCAST_TOTAL_SLICES);
    write_u16_be(s_packet_buf + 12, slice * SCREENCAST_SLICE_LINES);
    write_u16_be(s_packet_buf + 14, SCREENCAST_WIDTH);
}

static bool capture_slice(lv_display_t *disp, uint8_t slice_idx, uint8_t *out_buf, size_t buf_size)
{
    if (!disp || !out_buf || slice_idx >= SCREENCAST_TOTAL_SLICES) return false;

    if (!bsp_lvgl_lock(300)) {
        return false;
    }

    lv_obj_t *act_scr = lv_display_get_screen_active(disp);
    if (!act_scr) {
        bsp_lvgl_unlock();
        return false;
    }

    memset(out_buf, 0, buf_size);

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, SCREENCAST_WIDTH, SCREENCAST_SLICE_LINES,
                     LV_COLOR_FORMAT_RGB565, SCREENCAST_WIDTH * 2, out_buf, buf_size);
    lv_draw_buf_clear(&draw_buf, NULL);

    int32_t y_start = (int32_t)slice_idx * SCREENCAST_SLICE_LINES;
    int32_t y_end = y_start + SCREENCAST_SLICE_LINES - 1;

    lv_layer_t layer;
    lv_layer_init(&layer);
    layer.draw_buf = &draw_buf;
    layer.buf_area.x1 = 0;
    layer.buf_area.y1 = y_start;
    layer.buf_area.x2 = SCREENCAST_WIDTH - 1;
    layer.buf_area.y2 = y_end;
    layer.color_format = LV_COLOR_FORMAT_RGB565;
    layer._clip_area = layer.buf_area;
    layer.phy_clip_area = layer.buf_area;

    lv_draw_unit_send_event(NULL, LV_EVENT_CHILD_CREATED, &layer);

    lv_display_t *disp_old = lv_refr_get_disp_refreshing();
    lv_layer_t *layer_old = disp->layer_head;
    disp->layer_head = &layer;
    lv_refr_set_disp_refreshing(disp);

    // 1. 渲染主屏幕
    lv_obj_redraw(&layer, act_scr);

    // 2. 渲染 Top Layer（如状态栏）
    lv_obj_t *top_layer = lv_display_get_layer_top(disp);
    if (top_layer && lv_obj_get_child_count(top_layer) > 0) {
        lv_obj_redraw(&layer, top_layer);
    }

    // 3. 渲染 Sys Layer
    lv_obj_t *sys_layer = lv_display_get_layer_sys(disp);
    if (sys_layer && lv_obj_get_child_count(sys_layer) > 0) {
        lv_obj_redraw(&layer, sys_layer);
    }

    layer.all_tasks_added = true;
    while (layer.draw_task_head) {
        lv_draw_dispatch_wait_for_request();
        lv_draw_dispatch();
    }

    disp->layer_head = layer_old;
    lv_refr_set_disp_refreshing(disp_old);

    lv_draw_unit_send_event(NULL, LV_EVENT_SCREEN_LOAD_START, &layer);
    lv_draw_unit_send_event(NULL, LV_EVENT_CHILD_DELETED, &layer);

    bsp_lvgl_unlock();
    return true;
}

static void send_full_frame(void)
{
    if (!kiro_passport_network_is_connected()) {
        ESP_LOGW(TAG, "网络未就绪，放弃截屏");
        return;
    }

    lv_display_t *disp = lv_display_get_default();
    if (!disp) {
        return;
    }

    uint32_t seq = ++s_frame_seq;
    bool complete = true;
    int64_t total_start_us = esp_timer_get_time();
    int64_t capture_us = 0;
    int64_t send_us = 0;
    int64_t ack_us = 0;

    wifi_ps_type_t previous_ps = WIFI_PS_NONE;
    bool restore_wifi_ps = false;
    if (esp_wifi_get_ps(&previous_ps) == ESP_OK && previous_ps != WIFI_PS_NONE) {
        esp_err_t result = esp_wifi_set_ps(WIFI_PS_NONE);
        if (result == ESP_OK) {
            restore_wifi_ps = true;
        } else {
            ESP_LOGW(TAG, "截屏前关闭 Wi-Fi 省电失败: %s", esp_err_to_name(result));
        }
    }

    ESP_LOGI(TAG, "执行单帧截屏上报 seq=%lu (共 %d 个二进制包，整帧 ACK)",
             (unsigned long)seq, SCREENCAST_TOTAL_SLICES);

    xSemaphoreTake(s_ack_sem, 0);
    s_expected_ack_seq = seq;
    s_expected_ack_slice = SCREENCAST_TOTAL_SLICES - 1;

    // ponytail: one frame ACK is safe while send_binary is synchronous and TCP provides backpressure;
    // restore a bounded window if the network API becomes queued or asynchronous.
    for (uint8_t slice = 0; slice < SCREENCAST_TOTAL_SLICES; slice++) {
        if (!kiro_passport_network_is_connected()) {
            complete = false;
            break;
        }

        prepare_packet_header(seq, slice);
        int64_t phase_start_us = esp_timer_get_time();
        bool captured = capture_slice(disp, slice, s_packet_buf + SCREENCAST_HEADER_SIZE,
                                      SCREENCAST_RAW_SIZE);
        capture_us += esp_timer_get_time() - phase_start_us;
        if (!captured) {
            ESP_LOGW(TAG, "切片 %u 截取失败，中止本帧", slice);
            complete = false;
            break;
        }

        phase_start_us = esp_timer_get_time();
        int sent = kiro_passport_network_send_binary(s_packet_buf, SCREENCAST_PACKET_SIZE);
        send_us += esp_timer_get_time() - phase_start_us;
        if (sent != SCREENCAST_PACKET_SIZE) {
            ESP_LOGW(TAG, "切片 %u 发送失败 (%d/%d)，中止本帧",
                     slice, sent, SCREENCAST_PACKET_SIZE);
            complete = false;
            break;
        }
    }

    if (complete) {
        int64_t phase_start_us = esp_timer_get_time();
        if (xSemaphoreTake(s_ack_sem, pdMS_TO_TICKS(SCREENCAST_ACK_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "整帧等待云端确认超时");
            complete = false;
        }
        ack_us = esp_timer_get_time() - phase_start_us;
    }

    s_expected_ack_seq = UINT32_MAX;
    s_expected_ack_slice = UINT8_MAX;

    if (restore_wifi_ps) {
        esp_err_t result = esp_wifi_set_ps(previous_ps);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "截屏后恢复 Wi-Fi 省电失败: %s", esp_err_to_name(result));
        }
    }

    int64_t total_us = esp_timer_get_time() - total_start_us;
    ESP_LOGI(TAG, "截屏耗时 seq=%lu total=%lldms capture=%lldms send=%lldms ack=%lldms",
             (unsigned long)seq, (long long)(total_us / 1000),
             (long long)(capture_us / 1000), (long long)(send_us / 1000),
             (long long)(ack_us / 1000));
    if (complete) {
        ESP_LOGI(TAG, "单帧截屏 seq=%lu 上报完成", (unsigned long)seq);
    } else {
        ESP_LOGW(TAG, "单帧截屏 seq=%lu 上报未完成", (unsigned long)seq);
    }
}

static void screencast_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "按需远程截屏服务已就绪");

    while (1) {
        if (xSemaphoreTake(s_trigger_sem, portMAX_DELAY) == pdTRUE) {
            if (kiro_passport_network_is_connected()) {
                send_full_frame();
            } else {
                ESP_LOGW(TAG, "收到截屏请求，但 WebSocket 尚未就绪");
            }
        }
    }
}

esp_err_t screencast_init(void)
{
    if (s_trigger_sem) return ESP_OK;

    s_trigger_sem = xSemaphoreCreateBinary();
    s_ack_sem = xSemaphoreCreateBinary();
    if (!s_trigger_sem || !s_ack_sem) {
        ESP_LOGE(TAG, "创建截屏信号量失败");
        if (s_trigger_sem) vSemaphoreDelete(s_trigger_sem);
        if (s_ack_sem) vSemaphoreDelete(s_ack_sem);
        s_trigger_sem = NULL;
        s_ack_sem = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreate(screencast_task, "screencast", 6144, NULL, 4, &s_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建截屏任务失败");
        vSemaphoreDelete(s_trigger_sem);
        vSemaphoreDelete(s_ack_sem);
        s_trigger_sem = NULL;
        s_ack_sem = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "按需截屏服务初始化成功");
    return ESP_OK;
}

static bool s_screencast_enabled = true;

void screencast_set_enabled(bool enabled)
{
    s_screencast_enabled = enabled;
}

bool screencast_is_enabled(void)
{
    return s_screencast_enabled;
}

void screencast_request_capture(void)
{
    if (s_screencast_enabled && s_trigger_sem) {
        xSemaphoreGive(s_trigger_sem);
    }
}

void screencast_acknowledge(uint32_t frame_seq, uint8_t slice)
{
    if (s_ack_sem && frame_seq == s_expected_ack_seq && slice == s_expected_ack_slice) {
        xSemaphoreGive(s_ack_sem);
    }
}

