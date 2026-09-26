#pragma once
#include <stdint.h>

#define CHIP_ESP32S3 9
#define CHIP_FEATURE_WIFI_BGN (1 << 0)
#define CHIP_FEATURE_BLE (1 << 1)

typedef struct {
    int model;
    uint32_t features;
    uint16_t revision;
    uint8_t cores;
} esp_chip_info_t;

static inline void esp_chip_info(esp_chip_info_t *out_info)
{
    if (out_info) {
        out_info->model = CHIP_ESP32S3;
        out_info->features = CHIP_FEATURE_WIFI_BGN | CHIP_FEATURE_BLE;
        out_info->revision = 1;
        out_info->cores = 2;
    }
}
