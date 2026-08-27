/* simulator/module/module_entry.c
 * 固件模块入口：ABI 导出 + 垫片胶水。
 *
 * 本文件是"通用外壳 ↔ 固件模块"契约的模块侧实现：
 *  - 导出 sim_firmware_load()，外壳据此加载本模块；
 *  - main/ 源码调用到的 ESP-IDF 垫片函数（esp_log / freertos / nvs）在这里实现，
 *    全部转调外壳提供的服务表 sim_api_t。状态（NVS、日志链）由外壳持有，
 *    模块内不重复实现。
 */
#include "sim_api.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "module_internal.h"

#include <stdarg.h>
#include <stddef.h>

/* main/main.c 的固件入口 */
extern void app_main(void);

static const sim_api_t *g_api;

/* ==================== 胶水：esp_log / esp_err ==================== */
void esp_log_printf(int level, const char *tag, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    g_api->log_vprintf(level, tag, fmt, args);
    va_end(args);
}

vprintf_like_t esp_log_set_vprintf(vprintf_like_t func)
{
    return g_api->log_set_vprintf(func);
}

const char *esp_err_to_name(esp_err_t code)
{
    return g_api->esp_err_to_name(code);
}

/* ==================== 胶水：FreeRTOS 任务/队列 ==================== */
BaseType_t xTaskCreate(TaskFunction_t func, const char *name,
                       uint32_t stack_size, void *arg,
                       unsigned priority, TaskHandle_t *handle)
{
    void *created = NULL;
    BaseType_t result = (BaseType_t)g_api->task_create(func, name, stack_size,
                                                       arg, priority, &created);
    if (handle) *handle = created;
    return result;
}

void vTaskDelete(TaskHandle_t task)
{
    g_api->task_delete(task);
}

void vTaskDelay(TickType_t ticks)
{
    g_api->task_delay(ticks);
}

TickType_t xTaskGetTickCount(void)
{
    return (TickType_t)g_api->task_get_tick_count();
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    return g_api->queue_create(length, item_size);
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks)
{
    return (BaseType_t)g_api->queue_send(queue, item, ticks);
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks)
{
    return (BaseType_t)g_api->queue_receive(queue, item, ticks);
}

void vQueueDelete(QueueHandle_t queue)
{
    g_api->queue_delete(queue);
}

/* ==================== 胶水：NVS ==================== */
esp_err_t nvs_flash_init(void)
{
    return g_api->nvs_flash_init();
}

esp_err_t nvs_flash_erase(void)
{
    return g_api->nvs_flash_erase();
}

esp_err_t nvs_open(const char *namespace_name, uint32_t open_mode,
                   nvs_handle_t *out_handle)
{
    return g_api->nvs_open(namespace_name, open_mode, out_handle);
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key,
                       void *out_value, size_t *length)
{
    return g_api->nvs_get_blob(handle, key, out_value, length);
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                       const void *value, size_t length)
{
    return g_api->nvs_set_blob(handle, key, value, length);
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    return g_api->nvs_commit(handle);
}

void nvs_close(nvs_handle_t handle)
{
    g_api->nvs_close(handle);
}

/* ==================== 模块入口 ==================== */
static int module_start(const sim_api_t *api)
{
    if (!api || api->version != SIM_API_VERSION) return -1;
    g_api = api;
    app_main(); /* 固件启动：BSP/设置/菜单/状态栏全部复用 main/ 源码 */
    return 0;
}

static void module_frame(void)
{
    bsp_button_sim_poll(); /* 长按/连发状态机 */
    lv_timer_handler();    /* LVGL 渲染与定时器（LVGL 静态库在本模块内） */
}

static void module_key(int sdl_keycode, int down)
{
    bsp_button_sim_key(sdl_keycode, down != 0);
}

static void module_quit(void)
{
    /* P0：worker 线程为低频空转，随进程退出回收，无需清理 */
}

SIM_FIRMWARE_EXPORT const sim_firmware_exports_t *sim_firmware_load(void)
{
    static const sim_firmware_exports_t exports = {
        .abi_version = SIM_FIRMWARE_ABI_VERSION,
        .name = "ai-passport firmware (host build of main/)",
        .start = module_start,
        .frame = module_frame,
        .key = module_key,
        .quit = module_quit,
    };
    return &exports;
}
