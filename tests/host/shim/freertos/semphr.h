#pragma once
/* Shim for host-side tests: FreeRTOS mutex stand-ins backed by a real
 * pthread mutex (the pairing component is single-threaded on the host,
 * but the esp_timer shim fires callbacks synchronously and the mutex
 * must still behave). TEST-ONLY: never compiled into firmware. */

#include <stdbool.h>
#include <stdlib.h>
#include <pthread.h>

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(*m));
    if (m == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(m, NULL) != 0) {
        free(m);
        return NULL;
    }
    return (SemaphoreHandle_t)m;
}

static inline bool xSemaphoreTake(SemaphoreHandle_t h, uint32_t timeout)
{
    (void)timeout;
    return pthread_mutex_lock((pthread_mutex_t *)h) == 0;
}

static inline bool xSemaphoreGive(SemaphoreHandle_t h)
{
    return pthread_mutex_unlock((pthread_mutex_t *)h) == 0;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t h)
{
    pthread_mutex_destroy((pthread_mutex_t *)h);
    free(h);
}

#ifdef __cplusplus
}
#endif
