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
typedef int BaseType_t;

#define pdPASS ((BaseType_t)1)
#define pdFALSE ((BaseType_t)0)
#define pdTRUE ((BaseType_t)1)

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name,
                       uint32_t stack_bytes, void *arg, int priority,
                       TaskHandle_t *out);

/* Self-delete only (vTaskDelete(NULL) from inside the task, the
 * pattern michi_time uses). */
void vTaskDelete(TaskHandle_t task);

/* No-op stand-in (the pairing component's 50 ms wait only runs on a
 * full event queue, which the fake state bus never has). */
static inline void vTaskDelay(uint32_t ticks)
{
    (void)ticks;
}

#ifdef __cplusplus
}
#endif
