/* simulator/include/sim_api.h
 * PC 模拟器 ABI：通用外壳（shell）↔ 固件模块（firmware module）之间的唯一契约。
 *
 * 架构：外壳负责 SDL 窗口/事件泵、插件加载和基础服务（日志/FreeRTOS/NVS），
 * 固件模块（由 main/ 源码 + 宿主适配编译而成）在运行时被加载，通过本契约
 * 使用外壳的服务。两者都不得越界调用对方的符号——所有交互只走本头文件。
 *
 * 可插拔：换固件 = 替换 simulator/firmware/ 下的模块文件（.dll/.so/.dylib），
 * 外壳与固件都无需重新编译（前提是 ABI 版本兼容）。
 */
#pragma once

#include "esp_err.h"
#include "esp_log.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 服务表版本。任何一侧的 ABI 变更都必须递增并保持向后兼容策略明确。 */
#define SIM_API_VERSION 1

/* ---------------------------------------------------------------------------
 * 外壳 → 固件：服务表
 * 固件侧的垫片函数（esp_log_printf / xTaskCreate / nvs_* ...）由模块内的
 * 转发胶水实现，一律转调到这里。状态（NVS 存储、日志捕获链）由外壳持有，
 * 保证多模块/重载时数据唯一。
 * ------------------------------------------------------------------------- */
typedef struct sim_api_t {
    uint32_t version; /* 必须等于 SIM_API_VERSION */

    /* ---- 日志（与 IDF esp_log 语义一致，前缀/正文/换行由实现内部处理） ---- */
    void (*log_vprintf)(int level, const char *tag, const char *fmt, va_list args);
    vprintf_like_t (*log_set_vprintf)(vprintf_like_t func); /* 返回上一个 */
    const char *(*esp_err_to_name)(int code);

    /* ---- FreeRTOS 任务/队列（宿主实现见 shell/shim_freertos.c） ---- */
    int (*task_create)(void (*fn)(void *arg), const char *name, uint32_t stack_size,
                       void *arg, unsigned priority, void **handle);
    void (*task_delete)(void *handle);
    void (*task_delay)(uint32_t ticks);
    uint32_t (*task_get_tick_count)(void);
    void *(*queue_create)(uint32_t length, uint32_t item_size);
    int (*queue_send)(void *queue, const void *item, uint32_t ticks);
    int (*queue_receive)(void *queue, void *item, uint32_t ticks);
    void (*queue_delete)(void *queue);

    /* ---- NVS（宿主实现见 shell/shim_nvs.c，内存版，单次运行有效） ---- */
    esp_err_t (*nvs_flash_init)(void);
    esp_err_t (*nvs_flash_erase)(void);
    esp_err_t (*nvs_open)(const char *namespace_name, uint32_t open_mode,
                          uint32_t *out_handle);
    esp_err_t (*nvs_get_blob)(uint32_t handle, const char *key,
                              void *out_value, size_t *length);
    esp_err_t (*nvs_set_blob)(uint32_t handle, const char *key,
                              const void *value, size_t length);
    esp_err_t (*nvs_commit)(uint32_t handle);
    void (*nvs_close)(uint32_t handle);
} sim_api_t;

/* ---------------------------------------------------------------------------
 * 固件 → 外壳：模块导出（插件入口）
 * 模块必须导出 sim_firmware_load()，返回静态的导出表。
 * ------------------------------------------------------------------------- */
#define SIM_FIRMWARE_ABI_VERSION 1

typedef struct sim_firmware_exports_t {
    uint32_t abi_version;       /* 必须等于 SIM_FIRMWARE_ABI_VERSION */
    const char *name;           /* 模块名，用于日志 */
    /* 启动固件：校验服务表并调用固件的 app_main()。成功返回 0。 */
    int (*start)(const sim_api_t *api);
    /* 每帧回调：由外壳事件循环调用（推进按键连发状态机 + lv_timer_handler）。 */
    void (*frame)(void);
    /* 转发 SDL 按键事件（SDL_Keycode, down=1 按下）。 */
    void (*key)(int sdl_keycode, int down);
    /* 退出前回调（可选清理）。 */
    void (*quit)(void);
} sim_firmware_exports_t;

/* 模块必须导出的入口函数类型：sim_firmware_load() → 静态导出表 */
typedef const sim_firmware_exports_t *(*sim_firmware_load_fn)(void);

#if defined(_WIN32)
#define SIM_FIRMWARE_EXPORT __declspec(dllexport)
#else
#define SIM_FIRMWARE_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
}
#endif
