/* simulator/include/freertos/task.h
 * FreeRTOS 任务 API 宿主垫片，实现在 simulator/src/shim_freertos.c。
 */
#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *arg);

/* 创建 pthread 线程。stack_size/priority 在宿主机忽略。 */
BaseType_t xTaskCreate(TaskFunction_t func, const char *name,
                       uint32_t stack_size, void *arg,
                       unsigned priority, TaskHandle_t *handle);

/* 终止任务线程并回收。注意：与 IDF 不同，本实现会阻塞直到线程退出；
 * 因此不要在任务自身的中断/回调里调用。 */
void vTaskDelete(TaskHandle_t task);

/* 阻塞延迟（可被 vTaskDelete 中断）。 */
void vTaskDelay(TickType_t ticks);

/* 单调毫秒时钟，等价于 xTaskGetTickCount()。 */
TickType_t xTaskGetTickCount(void);

#define vTaskDelayMs(ms) vTaskDelay(pdMS_TO_TICKS(ms))

#ifdef __cplusplus
}
#endif
