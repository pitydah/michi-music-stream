#pragma once
#include <stdlib.h>
#include <stdint.h>

#define MALLOC_CAP_DMA (1 << 3)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)

__attribute__((weak)) void test_lcd_report_free(const void *ptr);

static inline void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}

static inline void heap_caps_free(void *ptr)
{
    if (test_lcd_report_free) {
        test_lcd_report_free(ptr);
    }
    free(ptr);
}
