#pragma once
/* Host shim for the ROM delay used by device-only timing. */
#include <stdint.h>
#include <time.h>

static inline void esp_rom_delay_us(uint32_t us)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)us * 1000L };
    nanosleep(&ts, NULL);
}
