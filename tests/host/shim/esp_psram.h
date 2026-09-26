#pragma once
#include <stddef.h>

static inline size_t esp_psram_get_size(void)
{
    return 8U * 1024U * 1024U;
}
