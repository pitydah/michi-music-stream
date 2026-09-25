#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "soc/gpio_num.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *i2s_chan_handle_t;

typedef enum {
    I2S_NUM_0 = 0,
    I2S_NUM_1 = 1,
} i2s_port_t;

typedef enum {
    I2S_ROLE_MASTER = 0,
    I2S_ROLE_SLAVE = 1,
} i2s_role_t;

typedef enum {
    I2S_DATA_BIT_WIDTH_16BIT = 16,
    I2S_DATA_BIT_WIDTH_24BIT = 24,
    I2S_DATA_BIT_WIDTH_32BIT = 32,
} i2s_data_bit_width_t;

typedef enum {
    I2S_SLOT_MODE_MONO = 1,
    I2S_SLOT_MODE_STEREO = 2,
} i2s_slot_mode_t;

#define I2S_GPIO_UNUSED (-1)

typedef struct {
    i2s_port_t id;
    i2s_role_t role;
    uint32_t dma_desc_num;
    uint32_t dma_frame_num;
    bool auto_clear;
    bool auto_clear_before_cb;
} i2s_chan_config_t;

#define I2S_CHANNEL_DEFAULT_CONFIG(port, role_val) { \
    .id = port, \
    .role = role_val, \
    .dma_desc_num = 6, \
    .dma_frame_num = 240, \
    .auto_clear = true, \
    .auto_clear_before_cb = false, \
}

typedef struct {
    uint32_t sample_rate_hz;
} i2s_std_clk_config_t;

#define I2S_STD_CLK_DEFAULT_CONFIG(rate) { .sample_rate_hz = rate }

typedef struct {
    i2s_data_bit_width_t data_bit_width;
    i2s_slot_mode_t slot_mode;
} i2s_std_slot_config_t;

#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode) { \
    .data_bit_width = bits, \
    .slot_mode = mode, \
}

typedef struct {
    gpio_num_t mclk;
    gpio_num_t bclk;
    gpio_num_t ws;
    gpio_num_t dout;
    struct { int unused; } invert_flags;
} i2s_std_gpio_config_t;

typedef struct {
    i2s_std_clk_config_t clk_cfg;
    i2s_std_slot_config_t slot_cfg;
    i2s_std_gpio_config_t gpio_cfg;
} i2s_std_config_t;

esp_err_t i2s_new_channel(const i2s_chan_config_t *chan_cfg, i2s_chan_handle_t *tx_handle, i2s_chan_handle_t *rx_handle);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t *std_cfg);
esp_err_t i2s_channel_enable(i2s_chan_handle_t handle);
esp_err_t i2s_channel_disable(i2s_chan_handle_t handle);
esp_err_t i2s_channel_write(i2s_chan_handle_t handle, const void *src, size_t size, size_t *bytes_written, uint32_t timeout_ms);
esp_err_t i2s_del_channel(i2s_chan_handle_t handle);

/* Test inspection hooks */
void test_i2s_reset(void);
size_t test_i2s_get_bytes_written(void);
uint32_t test_i2s_get_write_count(void);
bool test_i2s_last_write_was_silence(void);

#ifdef __cplusplus
}
#endif
