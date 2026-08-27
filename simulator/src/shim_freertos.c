/* simulator/src/shim_freertos.c
 * FreeRTOS 任务/队列的宿主实现（pthread）。
 *
 * 简化语义（P0）：
 *  - vTaskDelete 只释放句柄，不终止线程：worker 任务自身是"等待-检查"循环，
 *    删除队列后 xQueueReceive 退化为按超时睡眠返回 pdFALSE，线程以低频率空转。
 *    每进出一次游戏/音频页会残留一个空转线程，P1 再引入真正的任务取消。
 *  - 队列删除后（或句柄为 NULL）的接收行为是"睡眠 ticks 后返回 pdFALSE"，
 *    保证退出后 worker 不会空转烧 CPU，也不会访问已释放的队列。
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ 时钟 */
static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

TickType_t xTaskGetTickCount(void)
{
    return (TickType_t)now_ms();
}

static void sleep_ms(uint32_t ms)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------------ 任务 */
typedef struct {
    pthread_t tid;
} sim_task_t;

typedef struct {
    TaskFunction_t fn;
    void *arg;
} sim_task_arg_t;

static void *sim_task_entry(void *p)
{
    sim_task_arg_t arg = *(sim_task_arg_t *)p;
    free(p);
    arg.fn(arg.arg);
    return NULL;
}

BaseType_t xTaskCreate(TaskFunction_t func, const char *name,
                       uint32_t stack_size, void *arg,
                       unsigned priority, TaskHandle_t *handle)
{
    (void)name;
    (void)stack_size;
    (void)priority;

    sim_task_t *task = (sim_task_t *)calloc(1, sizeof(sim_task_t));
    sim_task_arg_t *start = (sim_task_arg_t *)malloc(sizeof(sim_task_arg_t));
    if (!task || !start) {
        free(task);
        free(start);
        return pdFAIL;
    }
    start->fn = func;
    start->arg = arg;

    if (pthread_create(&task->tid, NULL, sim_task_entry, start) != 0) {
        free(task);
        free(start);
        return pdFAIL;
    }
    if (handle) *handle = task;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task)
{
    /* P0：仅释放句柄。任务线程保持运行（空闲循环），见文件头注释。 */
    free(task);
}

void vTaskDelay(TickType_t ticks)
{
    sleep_ms((uint32_t)ticks);
}

/* ------------------------------------------------------------------ 队列 */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    uint8_t *items;
    uint32_t item_size;
    uint32_t capacity;
    uint32_t head;
    uint32_t count;
    bool deleted;
} sim_queue_t;

static void timedwait_until(pthread_cond_t *cond, pthread_mutex_t *lock,
                            uint32_t deadline_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += (time_t)(deadline_ms / 1000u);
    ts.tv_nsec += (long)(deadline_ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(cond, lock, &ts);
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    sim_queue_t *q = (sim_queue_t *)calloc(1, sizeof(sim_queue_t));
    if (!q) return NULL;
    q->items = (uint8_t *)malloc((size_t)length * item_size);
    if (!q->items) {
        free(q);
        return NULL;
    }
    q->item_size = item_size;
    q->capacity = length;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    return q;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks)
{
    sim_queue_t *q = (sim_queue_t *)queue;
    if (!q || q->deleted || !item) return pdFAIL;

    uint32_t deadline = now_ms() + (uint32_t)ticks;
    pthread_mutex_lock(&q->lock);
    while (q->count >= q->capacity && !q->deleted && now_ms() < deadline) {
        timedwait_until(&q->not_full, &q->lock, deadline);
    }
    if (q->deleted) {
        pthread_mutex_unlock(&q->lock);
        return pdFAIL;
    }
    if (q->count >= q->capacity) {
        pthread_mutex_unlock(&q->lock);
        return pdFAIL; /* 满且超时 */
    }
    uint32_t tail = (q->head + q->count) % q->capacity;
    memcpy(q->items + (size_t)tail * q->item_size, item, q->item_size);
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return pdPASS;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks)
{
    sim_queue_t *q = (sim_queue_t *)queue;

    /* 已删除/为 NULL 的队列：按超时睡眠后返回 pdFALSE，避免空转烧 CPU。 */
    if (!q || q->deleted) {
        sleep_ms((uint32_t)ticks);
        return pdFALSE;
    }

    uint32_t deadline = now_ms() + (uint32_t)ticks;
    pthread_mutex_lock(&q->lock);
    while (q->count == 0 && !q->deleted && now_ms() < deadline) {
        timedwait_until(&q->not_empty, &q->lock, deadline);
    }
    if (q->count == 0 || q->deleted) {
        pthread_mutex_unlock(&q->lock);
        return pdFALSE;
    }
    if (item) {
        memcpy(item, q->items + (size_t)q->head * q->item_size, q->item_size);
    }
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return pdPASS;
}

void vQueueDelete(QueueHandle_t queue)
{
    sim_queue_t *q = (sim_queue_t *)queue;
    if (!q) return;
    /* 只标记删除并唤醒等待者，不释放内存：worker 线程可能仍持有该指针
     * （如 game_audio 的 s_sfx_queue），释放会造成 use-after-free。
     * P0 队列数量极少，随进程退出回收即可。 */
    pthread_mutex_lock(&q->lock);
    q->deleted = true;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);
}
