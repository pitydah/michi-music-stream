#pragma once
/* Shim for host-side tests: minimal FreeRTOS stand-ins. The pairing
 * component uses only a mutex (semphr.h), vTaskDelay (task.h) and the
 * portMAX_DELAY/pdMS_TO_TICKS macros. TEST-ONLY: never compiled into
 * firmware. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define portMAX_DELAY (UINT32_MAX)
#define pdMS_TO_TICKS(ms) ((uint32_t)(ms))

#ifndef pdTRUE
#define pdTRUE 1
#endif
#ifndef pdFALSE
#define pdFALSE 0
#endif
#ifndef pdPASS
#define pdPASS 1
#endif
#ifndef pdFAIL
#define pdFAIL 0
#endif

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;

#include <pthread.h>
typedef struct { pthread_mutex_t m; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { PTHREAD_MUTEX_INITIALIZER }

extern __thread uint32_t s_freertos_critical_depth;
uint32_t test_freertos_api_in_critical_count(void);
void test_freertos_api_reset_in_critical_count(void);
#ifndef MICHI_SHIM_TASK_IMPL
__attribute__((weak)) void test_freertos_check_critical(const char *api_name)
{
    (void)api_name;
}
#else
void test_freertos_check_critical(const char *api_name);
#endif

static inline void portENTER_CRITICAL_shim(portMUX_TYPE *mux)
{
    pthread_mutex_lock(&((portMUX_TYPE *)(mux))->m);
    s_freertos_critical_depth++;
}

static inline void portEXIT_CRITICAL_shim(portMUX_TYPE *mux)
{
    if (s_freertos_critical_depth > 0) {
        s_freertos_critical_depth--;
    }
    pthread_mutex_unlock(&((portMUX_TYPE *)(mux))->m);
}

#define portENTER_CRITICAL(mux) portENTER_CRITICAL_shim((portMUX_TYPE *)(mux))
#define portEXIT_CRITICAL(mux) portEXIT_CRITICAL_shim((portMUX_TYPE *)(mux))
#define portENTER_CRITICAL_ISR(mux) portENTER_CRITICAL_shim((portMUX_TYPE *)(mux))
#define portEXIT_CRITICAL_ISR(mux) portEXIT_CRITICAL_shim((portMUX_TYPE *)(mux))

#ifdef __cplusplus
}
#endif

