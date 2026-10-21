/* Monotonic host time for RTP worker; unlike timer fake it advances naturally. */
#include <stdint.h>
#include <time.h>
#include "esp_timer.h"
int64_t esp_timer_get_time(void)
{
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * INT64_C(1000000) + (int64_t)ts.tv_nsec / 1000;
}
