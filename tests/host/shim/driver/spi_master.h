#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SPI2_HOST 1
#define SPI_DMA_CH_AUTO 3

typedef struct {
    int mosi_io_num;
    int miso_io_num;
    int sclk_io_num;
    int quadwp_io_num;
    int quadhd_io_num;
    size_t max_transfer_sz;
} spi_bus_config_t;

static inline esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *bus_config, int dma_chan)
{
    (void)host;
    (void)bus_config;
    (void)dma_chan;
    return ESP_OK;
}

static inline esp_err_t spi_bus_free(int host)
{
    (void)host;
    return ESP_OK;
}
