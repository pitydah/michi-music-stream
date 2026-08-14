#pragma once
/* Shim for host-side tests: vTaskDelay stand-in (a 50 ms wait only runs
 * on a full event queue, which the fake state bus never has). TEST-ONLY:
 * never compiled into firmware. */

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline void vTaskDelay(uint32_t ticks)
{
    (void)ticks;
}

#ifdef __cplusplus
}
#endif
