#include "queue.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

static bool s_force_send_fail = false;
void test_queue_set_force_send_fail(bool fail) { s_force_send_fail = fail; }

struct michi_shim_queue {
    pthread_mutex_t m;
    pthread_cond_t cv;
    uint8_t *data;
    uint32_t capacity;
    uint32_t item_size;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
};

QueueHandle_t xQueueCreate(uint32_t uxQueueLength, uint32_t uxItemSize) {
    struct michi_shim_queue *q = calloc(1, sizeof(*q));
    if (!q) return NULL;
    q->capacity = uxQueueLength;
    q->item_size = uxItemSize;
    q->data = calloc(uxQueueLength, uxItemSize);
    pthread_mutex_init(&q->m, NULL);
    pthread_cond_init(&q->cv, NULL);
    return q;
}

BaseType_t xQueueSendFromISR(QueueHandle_t xQueue, const void *pvItemToQueue, BaseType_t *pxHigherPriorityTaskWoken) {
    if (pxHigherPriorityTaskWoken) *pxHigherPriorityTaskWoken = pdTRUE;
    pthread_mutex_lock(&xQueue->m);
    if (xQueue->count == xQueue->capacity) {
        pthread_mutex_unlock(&xQueue->m);
        return pdFALSE;
    }
    memcpy(xQueue->data + xQueue->tail * xQueue->item_size, pvItemToQueue, xQueue->item_size);
    xQueue->tail = (xQueue->tail + 1) % xQueue->capacity;
    xQueue->count++;
    pthread_cond_signal(&xQueue->cv);
    pthread_mutex_unlock(&xQueue->m);
    return pdTRUE;
}

BaseType_t xQueueSend(QueueHandle_t xQueue, const void *pvItemToQueue, uint32_t xTicksToWait) {
    (void)xTicksToWait;
    if (s_force_send_fail) return pdFALSE; /* non-blocking in shim: mirrors xQueueSendFromISR behaviour */
    pthread_mutex_lock(&xQueue->m);
    if (xQueue->count == xQueue->capacity) {
        pthread_mutex_unlock(&xQueue->m);
        return pdFALSE;
    }
    memcpy(xQueue->data + xQueue->tail * xQueue->item_size, pvItemToQueue, xQueue->item_size);
    xQueue->tail = (xQueue->tail + 1) % xQueue->capacity;
    xQueue->count++;
    pthread_cond_signal(&xQueue->cv);
    pthread_mutex_unlock(&xQueue->m);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t xQueue, void *pvBuffer, uint32_t xTicksToWait) {
    pthread_mutex_lock(&xQueue->m);
    struct timespec deadline = {0};
    if (xTicksToWait != 0 && xTicksToWait != portMAX_DELAY) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        uint64_t ns = (uint64_t)deadline.tv_nsec +
                      (uint64_t)xTicksToWait * 1000000ULL;
        deadline.tv_sec += (time_t)(ns / 1000000000ULL);
        deadline.tv_nsec = (long)(ns % 1000000000ULL);
    }
    while (xQueue->count == 0) {
        if (xTicksToWait == 0) {
            pthread_mutex_unlock(&xQueue->m);
            return pdFALSE;
        }
        int rc = xTicksToWait == portMAX_DELAY
               ? pthread_cond_wait(&xQueue->cv, &xQueue->m)
               : pthread_cond_timedwait(&xQueue->cv, &xQueue->m, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&xQueue->m);
            return pdFALSE;
        }
        if (rc != 0) {
            pthread_mutex_unlock(&xQueue->m);
            return pdFALSE;
        }
    }
    memcpy(pvBuffer, xQueue->data + xQueue->head * xQueue->item_size, xQueue->item_size);
    xQueue->head = (xQueue->head + 1) % xQueue->capacity;
    xQueue->count--;
    pthread_mutex_unlock(&xQueue->m);
    return pdTRUE;
}

void vQueueDelete(QueueHandle_t xQueue) {
    if (!xQueue) return;
    free(xQueue->data);
    free(xQueue);
}
