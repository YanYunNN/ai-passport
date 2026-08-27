/* simulator/include/freertos/queue.h
 * FreeRTOS 队列 API 宿主垫片，实现在 simulator/src/shim_freertos.c。
 */
#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);

/* 非阻塞/超时发送。队列已满且超时=0 时返回 pdFALSE。 */
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks);

/* 阻塞接收。超时到期返回 pdFALSE；队列句柄为 NULL（已删除）时立即返回 pdFALSE。 */
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks);

/* 删除队列。已删除/为 NULL 的队列上的后续接收立即返回 pdFALSE。 */
void vQueueDelete(QueueHandle_t queue);

#ifdef __cplusplus
}
#endif
