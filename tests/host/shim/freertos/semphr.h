#pragma once
/* Shim for host-side tests: FreeRTOS semaphore stand-ins.
 *
 * Two kinds, distinguished by a tag inside the handle:
 *  - mutexes (xSemaphoreCreateMutex): backed by a real pthread mutex
 *    (the pairing/session components are single-threaded on the host,
 *    but callbacks fire synchronously and the mutex must still behave).
 *  - binary semaphores (xSemaphoreCreateBinary): mutex + condvar with a
 *    0/1 count - give on a full semaphore fails silently (FreeRTOS
 *    semantics), take blocks until a give (bounded by the timeout).
 *
 * TEST-ONLY: never compiled into firmware. */

#include <stdbool.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct michi_shim_sem {
    bool binary;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
} michi_shim_sem_t;

typedef michi_shim_sem_t *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    michi_shim_sem_t *s = (michi_shim_sem_t *)calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->binary = false;
    if (pthread_mutex_init(&s->mutex, NULL) != 0) {
        free(s);
        return NULL;
    }
    return s;
}

static inline SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    michi_shim_sem_t *s = (michi_shim_sem_t *)calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->binary = true;
    s->count = 0;
    if (pthread_mutex_init(&s->mutex, NULL) != 0 ||
        pthread_cond_init(&s->cond, NULL) != 0) {
        free(s);
        return NULL;
    }
    return s;
}

static inline bool xSemaphoreTake(SemaphoreHandle_t h, uint32_t timeout)
{
    if (h == NULL) {
        return false;
    }
    if (!h->binary) {
        /* Mutex semantics: lock and report. */
        return pthread_mutex_lock(&h->mutex) == 0;
    }
    pthread_mutex_lock(&h->mutex);
    if (h->count > 0) {
        h->count = 0;
        pthread_mutex_unlock(&h->mutex);
        return true;
    }
    if (timeout == portMAX_DELAY) {
        while (h->count == 0) {
            pthread_cond_wait(&h->cond, &h->mutex);
        }
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)(timeout / 1000);
        ts.tv_nsec += (long)(timeout % 1000) * 1000000L;
        while (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        while (h->count == 0) {
            if (pthread_cond_timedwait(&h->cond, &h->mutex, &ts) != 0) {
                pthread_mutex_unlock(&h->mutex);
                return false; /* timeout */
            }
        }
    }
    h->count = 0;
    pthread_mutex_unlock(&h->mutex);
    return true;
}

static inline bool xSemaphoreGive(SemaphoreHandle_t h)
{
    if (h == NULL) {
        return false;
    }
    if (!h->binary) {
        return pthread_mutex_unlock(&h->mutex) == 0;
    }
    pthread_mutex_lock(&h->mutex);
    if (h->count == 0) {
        h->count = 1;
        pthread_cond_broadcast(&h->cond);
    }
    pthread_mutex_unlock(&h->mutex);
    return true;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t h)
{
    /* TEST-ONLY contract: callers never delete a semaphore while a task
     * is blocked on it (michi_time joins its task first). */
    if (h == NULL) {
        return;
    }
    if (h->binary) {
        pthread_cond_destroy(&h->cond);
    }
    pthread_mutex_destroy(&h->mutex);
    free(h);
}

#ifdef __cplusplus
}
#endif
