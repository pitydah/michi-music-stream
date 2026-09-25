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
    usleep((useconds_t)ticks * 1000);
}

static inline void xTaskNotifyGive(TaskHandle_t task)
{
    (void)task;
}

static inline uint32_t ulTaskNotifyTake(BaseType_t clear_count, uint32_t ticks)
{
    (void)clear_count;
    if (ticks > 0) {
        usleep((useconds_t)(ticks > 5 ? 5 : ticks) * 1000);
    }
    return 1;
}

#ifdef __cplusplus
}
#endif

/* Shim for portYIELD_FROM_ISR */
static inline void portYIELD_FROM_ISR(void) {
    /* No-op in shim */
}
