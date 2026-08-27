/* simulator/include/freertos/FreeRTOS.h
 * FreeRTOS 宿主垫片（精简）：只覆盖 main/ 编译单元实际用到的宏与类型。
 * 真实任务/队列实现在 simulator/src/shim_freertos.c（基于 pthread）。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define configTICK_RATE_HZ 1000u
#define portTICK_PERIOD_MS ((TickType_t)1000 / configTICK_RATE_HZ)

typedef uint32_t TickType_t;
typedef int      BaseType_t;
typedef uint32_t UBaseType_t;

#define pdFALSE ((BaseType_t)0)
#define pdTRUE  ((BaseType_t)1)
#define pdPASS  ((BaseType_t)1)
#define pdFAIL  ((BaseType_t)0)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms) * configTICK_RATE_HZ / 1000u)
#define portMAX_DELAY ((TickType_t)0xffffffffUL)

/* 临界区：宿主机单线程 LVGL 主循环 + 独立音频任务，debug_log 用它保护日志环形缓冲。
 * 退化为空操作（日志线程竞争可忽略，P0 可接受）。 */
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(mux) do { (void)(mux); } while (0)
#define taskEXIT_CRITICAL(mux)  do { (void)(mux); } while (0)

#ifdef __cplusplus
}
#endif
