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

static SemaphoreHandle_t s_trigger_sem = NULL;
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
        ESP_LOGW(TAG, "网络未就绪，放弃截屏");
        return;
    }

    lv_display_t *disp = lv_display_get_default();
    if (!disp) {
        return;
    }

    uint32_t seq = ++s_frame_seq;
    ESP_LOGI(TAG, "执行单帧截屏上报 seq=%lu (共 %d 片)", (unsigned long)seq, SCREENCAST_TOTAL_SLICES);

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
                ESP_LOGW(TAG, "切片 %u 发送受阻，中止本帧以保护连接", slice);
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(15));
    }
    ESP_LOGI(TAG, "单帧截屏 seq=%lu 上报完成", (unsigned long)seq);
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
    if (!s_trigger_sem) {
        ESP_LOGE(TAG, "创建触发信号量失败");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreate(screencast_task, "screencast", 6144, NULL, 4, &s_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建截屏任务失败");
        vSemaphoreDelete(s_trigger_sem);
        s_trigger_sem = NULL;
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

