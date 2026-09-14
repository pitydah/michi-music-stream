#include "queue.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

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

BaseType_t xQueueReceive(QueueHandle_t xQueue, void *pvBuffer, uint32_t xTicksToWait) {
    pthread_mutex_lock(&xQueue->m);
    while (xQueue->count == 0) {
        if (xTicksToWait == 0) {
            pthread_mutex_unlock(&xQueue->m);
            return pdFALSE;
        }
        pthread_cond_wait(&xQueue->cv, &xQueue->m);
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
