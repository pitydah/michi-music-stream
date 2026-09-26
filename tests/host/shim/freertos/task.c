/* Shim for host-side tests: pthread-backed FreeRTOS tasks (see task.h).
 * The task struct is intentionally leaked (test-only, few tasks per
 * binary). TEST-ONLY: never compiled into firmware. */

#define MICHI_SHIM_TASK_IMPL 1
#include "task.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

typedef enum {
    SHIM_TASK_ALIVE = 1,
    SHIM_TASK_SELF_DELETED,
    SHIM_TASK_JOINED_OR_RETIRED,
} shim_task_lifecycle_t;

struct michi_shim_task {
    pthread_t thread;
    TaskFunction_t fn;
    void *arg;
    pthread_mutex_t notify_mux;
    pthread_cond_t notify_cond;
    uint32_t notify_value;
    bool has_notification;
    shim_task_lifecycle_t state;
};

static uint32_t s_invalid_notify_count = 0;
static uint32_t s_external_delete_count = 0;

__thread uint32_t s_freertos_critical_depth = 0;
static uint32_t s_freertos_api_in_critical_count = 0;

uint32_t test_freertos_api_in_critical_count(void)
{
    return s_freertos_api_in_critical_count;
}

void test_freertos_api_reset_in_critical_count(void)
{
    s_freertos_api_in_critical_count = 0;
}

void test_freertos_check_critical(const char *api_name)
{
    if (s_freertos_critical_depth > 0) {
        s_freertos_api_in_critical_count++;
        fprintf(stderr, "HOST SHIM VIOLATION: FreeRTOS API '%s' called inside critical section (depth=%u)!\n",
                api_name, (unsigned)s_freertos_critical_depth);
    }
}

uint32_t test_task_invalid_notify_count(void)
{
    return s_invalid_notify_count;
}

void test_task_reset_invalid_notify_count(void)
{
    s_invalid_notify_count = 0;
}

uint32_t test_task_external_delete_count(void)
{
    return s_external_delete_count;
}

void test_task_reset_external_delete_count(void)
{
    s_external_delete_count = 0;
}

static __thread michi_shim_task_t *s_current_task = NULL;

static void *task_entry(void *arg)
{
    michi_shim_task_t *t = (michi_shim_task_t *)arg;
    s_current_task = t;
    t->fn(t->arg);
    return NULL;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return s_current_task;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name,
                       uint32_t stack_bytes, void *arg, int priority,
                       TaskHandle_t *out)
{
    (void)name;
    (void)stack_bytes;
    (void)priority;
    if (fn == NULL || out == NULL) {
        return pdFALSE;
    }
    michi_shim_task_t *t = (michi_shim_task_t *)calloc(1, sizeof(*t));
    if (t == NULL) {
        return pdFALSE;
    }
    t->fn = fn;
    t->arg = arg;
    pthread_mutex_init(&t->notify_mux, NULL);
    pthread_cond_init(&t->notify_cond, NULL);
    t->notify_value = 0;
    t->has_notification = false;
    t->state = SHIM_TASK_ALIVE;

    if (pthread_create(&t->thread, NULL, task_entry, t) != 0) {
        pthread_mutex_destroy(&t->notify_mux);
        pthread_cond_destroy(&t->notify_cond);
        free(t);
        return pdFALSE;
    }
    *out = t;
    return pdPASS;
}

BaseType_t xTaskNotify(TaskHandle_t task, uint32_t ulValue, eNotifyAction eAction)
{
    test_freertos_check_critical("xTaskNotify");
    if (task == NULL) {
        return pdFAIL;
    }

    michi_shim_task_t *t = (michi_shim_task_t *)task;
    pthread_mutex_lock(&t->notify_mux);
    if (t->state != SHIM_TASK_ALIVE) {
        s_invalid_notify_count++;
        fprintf(stderr, "HOST SHIM ERROR: xTaskNotify called on stale/dead task %p (state=%d)!\n",
                (void *)task, (int)t->state);
        pthread_mutex_unlock(&t->notify_mux);
        return pdFAIL;
    }
    if (eAction == eSetBits) {
        t->notify_value |= ulValue;
    } else if (eAction == eSetValueWithOverwrite) {
        t->notify_value = ulValue;
    } else if (eAction == eIncrement) {
        t->notify_value++;
    }
    t->has_notification = true;
    pthread_cond_broadcast(&t->notify_cond);
    pthread_mutex_unlock(&t->notify_mux);
    return pdPASS;
}

BaseType_t xTaskNotifyFromISR(TaskHandle_t task, uint32_t ulValue, eNotifyAction eAction,
                              BaseType_t *pxHigherPriorityTaskWoken)
{
    if (pxHigherPriorityTaskWoken) {
        *pxHigherPriorityTaskWoken = pdFALSE;
    }
    return xTaskNotify(task, ulValue, eAction);
}

BaseType_t xTaskNotifyWait(uint32_t ulBitsToClearOnEntry, uint32_t ulBitsToClearOnExit,
                           uint32_t *pulNotificationValue, TickType_t xTicksToWait)
{
    test_freertos_check_critical("xTaskNotifyWait");
    michi_shim_task_t *t = s_current_task;
    if (t == NULL) {
        return pdFALSE;
    }
    pthread_mutex_lock(&t->notify_mux);
    if (ulBitsToClearOnEntry != 0) {
        t->notify_value &= ~ulBitsToClearOnEntry;
    }

    if (!t->has_notification && xTicksToWait > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t ns = (uint64_t)ts.tv_nsec + (uint64_t)xTicksToWait * 1000000ULL;
        ts.tv_sec += (time_t)(ns / 1000000000ULL);
        ts.tv_nsec = (long)(ns % 1000000000ULL);
        while (!t->has_notification) {
            int rc = pthread_cond_timedwait(&t->notify_cond, &t->notify_mux, &ts);
            if (rc != 0) {
                break;
            }
        }
    }

    BaseType_t res = pdFALSE;
    if (t->has_notification) {
        if (pulNotificationValue != NULL) {
            *pulNotificationValue = t->notify_value;
        }
        t->notify_value &= ~ulBitsToClearOnExit;
        t->has_notification = (t->notify_value != 0);
        res = pdTRUE;
    }
    pthread_mutex_unlock(&t->notify_mux);
    return res;
}

uint32_t ulTaskNotifyTake(BaseType_t clear_count, TickType_t ticks)
{
    test_freertos_check_critical("ulTaskNotifyTake");
    michi_shim_task_t *t = s_current_task;

    if (t == NULL) {
        return 0;
    }
    pthread_mutex_lock(&t->notify_mux);
    if (!t->has_notification && ticks > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t ns = (uint64_t)ts.tv_nsec + (uint64_t)ticks * 1000000ULL;
        ts.tv_sec += (time_t)(ns / 1000000000ULL);
        ts.tv_nsec = (long)(ns % 1000000000ULL);
        while (!t->has_notification) {
            int rc = pthread_cond_timedwait(&t->notify_cond, &t->notify_mux, &ts);
            if (rc != 0) {
                break;
            }
        }
    }
    uint32_t res = t->notify_value;
    if (res > 0) {
        if (clear_count) {
            t->notify_value = 0;
            t->has_notification = false;
        } else {
            t->notify_value--;
            t->has_notification = (t->notify_value != 0);
        }
    }
    pthread_mutex_unlock(&t->notify_mux);
    return res;
}

void vTaskDelete(TaskHandle_t task)
{
    if (task == NULL) {
        if (s_current_task != NULL) {
            pthread_mutex_lock(&s_current_task->notify_mux);
            s_current_task->state = SHIM_TASK_SELF_DELETED;
            pthread_mutex_unlock(&s_current_task->notify_mux);
        }
        pthread_exit(NULL);
    } else {
        s_external_delete_count++;
        michi_shim_task_t *t = (michi_shim_task_t *)task;
        pthread_mutex_lock(&t->notify_mux);
        t->state = SHIM_TASK_JOINED_OR_RETIRED;
        pthread_mutex_unlock(&t->notify_mux);
        pthread_join(t->thread, NULL);
        pthread_mutex_destroy(&t->notify_mux);
        pthread_cond_destroy(&t->notify_cond);
        free(t);
    }
}
