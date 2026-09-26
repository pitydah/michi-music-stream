#pragma once
#include <stdint.h>
#include "esp_err.h"

typedef void *esp_flash_t;

static inline esp_err_t esp_flash_get_size(esp_flash_t *chip, uint32_t *out_size)
{
    (void)chip;
    if (out_size) {
        *out_size = 16U * 1024U * 1024U;
    }
    return ESP_OK;
}
