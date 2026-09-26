#pragma once
/* Shim for host-side tests: FreeRTOS task stand-ins backed by pthreads.
 * Supports exactly what michi_time needs: xTaskCreate (spawns the sync
 * task), vTaskDelete(NULL) (self-delete from inside the task). The task
 * struct is intentionally leaked (test-only). TEST-ONLY: never compiled
 * into firmware. */

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*TaskFunction_t)(void *arg);
typedef struct michi_shim_task michi_shim_task_t;
typedef michi_shim_task_t *TaskHandle_t;

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name,
                       uint32_t stack_bytes, void *arg, int priority,
                       TaskHandle_t *out);

/* Self-delete only (vTaskDelete(NULL) from inside the task, the
 * pattern michi_time uses). */
void vTaskDelete(TaskHandle_t task);

#include <unistd.h>

/* Real delay for host shim so cooperative yields/waits work. */
static inline void vTaskDelay(uint32_t ticks)
{
    test_freertos_check_critical("vTaskDelay");
    usleep((useconds_t)ticks * 1000);
}


typedef enum {
    eNoAction = 0,
    eSetBits,
    eIncrement,
    eSetValueWithOverwrite,
    eSetValueWithoutOverwrite
} eNotifyAction;

BaseType_t xTaskNotify(TaskHandle_t xTaskToNotify, uint32_t ulValue, eNotifyAction eAction);
BaseType_t xTaskNotifyFromISR(TaskHandle_t xTaskToNotify, uint32_t ulValue, eNotifyAction eAction,
                              BaseType_t *pxHigherPriorityTaskWoken);
BaseType_t xTaskNotifyWait(uint32_t ulBitsToClearOnEntry, uint32_t ulBitsToClearOnExit,
                           uint32_t *pulNotificationValue, TickType_t xTicksToWait);
TaskHandle_t xTaskGetCurrentTaskHandle(void);

static inline void xTaskNotifyGive(TaskHandle_t task)
{
    (void)xTaskNotify(task, 0, eIncrement);
}

uint32_t ulTaskNotifyTake(BaseType_t clear_count, TickType_t ticks);

/* Test diagnostic hooks to detect stale task handle usage and external delete */
uint32_t test_task_invalid_notify_count(void);
void test_task_reset_invalid_notify_count(void);
uint32_t test_task_external_delete_count(void);
void test_task_reset_external_delete_count(void);

#ifdef __cplusplus
}
#endif

/* Shim for portYIELD_FROM_ISR */
static inline void portYIELD_FROM_ISR(void) {
    /* No-op in shim */
}
