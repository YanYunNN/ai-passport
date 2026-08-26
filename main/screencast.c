#include "screencast.h"
#include "bsp_display.h"
#include "kiro_passport_network.h"
#include "lvgl.h"
#include "display/lv_display_private.h"
#include "core/lv_refr_private.h"
#include "mbedtls/base64.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "screencast";

#define SCREENCAST_WIDTH        240
#define SCREENCAST_HEIGHT       320
#define SCREENCAST_SLICE_LINES  5
#define SCREENCAST_TOTAL_SLICES (SCREENCAST_HEIGHT / SCREENCAST_SLICE_LINES) // 64
#define SCREENCAST_RAW_SIZE     (SCREENCAST_WIDTH * SCREENCAST_SLICE_LINES * 2) // 2400 bytes
#define SCREENCAST_B64_SIZE     (((SCREENCAST_RAW_SIZE + 2) / 3) * 4 + 4) // 3204 bytes
#define SCREENCAST_MSG_SIZE     (SCREENCAST_B64_SIZE + 256) // ~3460 bytes

static bool s_enabled = false;
static uint32_t s_interval_ms = 1000;
static bool s_capture_requested = false;
static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_task_handle = NULL;
static uint32_t s_frame_seq = 0;

static uint8_t s_raw_buf[SCREENCAST_RAW_SIZE];
static char s_b64_buf[SCREENCAST_B64_SIZE];
static char s_msg_buf[SCREENCAST_MSG_SIZE];

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
        return;
    }

    lv_display_t *disp = lv_display_get_default();
    if (!disp) {
        return;
    }

    uint32_t seq = ++s_frame_seq;
    ESP_LOGI(TAG, "正在采集并发送全屏帧 seq=%lu (共 %d 片)", (unsigned long)seq, SCREENCAST_TOTAL_SLICES);

    for (uint8_t slice = 0; slice < SCREENCAST_TOTAL_SLICES; slice++) {
        if (!kiro_passport_network_is_connected()) break;

        if (!capture_slice(disp, slice, s_raw_buf, SCREENCAST_RAW_SIZE)) {
            continue;
        }

        size_t b64_len = 0;
        int ret = mbedtls_base64_encode((unsigned char *)s_b64_buf, SCREENCAST_B64_SIZE - 1,
                                        &b64_len, s_raw_buf, SCREENCAST_RAW_SIZE);
        if (ret != 0) {
            continue;
        }
        s_b64_buf[b64_len] = '\0';

        int len = snprintf(s_msg_buf, SCREENCAST_MSG_SIZE,
            "{\"v\":1,\"type\":\"screencast\",\"seq\":%lu,\"slice\":%u,\"total\":%u,\"y\":%u,\"lines\":%u,\"data\":\"%s\"}",
            (unsigned long)seq, (unsigned int)slice, (unsigned int)SCREENCAST_TOTAL_SLICES,
            (unsigned int)(slice * SCREENCAST_SLICE_LINES), (unsigned int)SCREENCAST_SLICE_LINES,
            s_b64_buf);

        if (len > 0) {
            int sent = kiro_passport_network_send_text(s_msg_buf);
            if (sent < 0) {
                ESP_LOGW(TAG, "切片 %u 发送受阻，中止本帧投屏以保护连接", slice);
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(15));
    }
    ESP_LOGD(TAG, "全屏帧 seq=%lu 发送完成", (unsigned long)seq);
}

static void screencast_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "实时投屏采集任务已启动");

    while (1) {
        bool should_run = false;

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_capture_requested) {
                s_capture_requested = false;
                should_run = true;
            } else if (s_enabled) {
                should_run = true;
            }
            xSemaphoreGive(s_lock);
        }

        if (should_run) {
            if (kiro_passport_network_is_connected()) {
                send_full_frame();
            } else {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }

        uint32_t delay_ms = s_interval_ms;
        if (delay_ms < 1500) delay_ms = 1500;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

esp_err_t screencast_init(void)
{
    if (s_lock) return ESP_OK;

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "创建互斥锁失败");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreate(screencast_task, "screencast", 3072, NULL, 4, &s_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建投屏任务失败");
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "实时投屏服务初始化成功");
    return ESP_OK;
}

void screencast_set_enabled(bool enabled)
{
    if (s_lock && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_enabled = enabled;
        ESP_LOGI(TAG, "实时投屏已%s", enabled ? "开启" : "关闭");
        xSemaphoreGive(s_lock);
    }
}

bool screencast_is_enabled(void)
{
    bool enabled = false;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        enabled = s_enabled;
        xSemaphoreGive(s_lock);
    }
    return enabled;
}

void screencast_request_capture(void)
{
    if (s_lock && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_capture_requested = true;
        ESP_LOGI(TAG, "已触发单帧远程截屏请求");
        xSemaphoreGive(s_lock);
    }
}

void screencast_set_interval_ms(uint32_t interval_ms)
{
    if (s_lock && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_interval_ms = interval_ms;
        xSemaphoreGive(s_lock);
    }
}
