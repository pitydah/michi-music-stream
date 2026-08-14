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

#ifdef __cplusplus
}
#endif
