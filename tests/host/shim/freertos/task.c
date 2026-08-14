/* Shim for host-side tests: pthread-backed FreeRTOS tasks (see task.h).
 * The task struct is intentionally leaked (test-only, few tasks per
 * binary). TEST-ONLY: never compiled into firmware. */

#include "task.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct michi_shim_task {
    pthread_t thread;
    TaskFunction_t fn;
    void *arg;
};

static void *task_entry(void *arg)
{
    michi_shim_task_t *t = (michi_shim_task_t *)arg;
    t->fn(t->arg);
    return NULL;
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
    if (pthread_create(&t->thread, NULL, task_entry, t) != 0) {
        free(t);
        return pdFALSE;
    }
    pthread_detach(t->thread);
    *out = t;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task)
{
    if (task == NULL) {
        /* Self-delete: the creator detached the thread; the handle
         * struct is intentionally leaked (test-only). */
        pthread_exit(NULL);
    }
    (void)task;
}
