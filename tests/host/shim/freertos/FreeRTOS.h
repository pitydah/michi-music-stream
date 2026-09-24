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

#ifdef __cplusplus
}
#endif
