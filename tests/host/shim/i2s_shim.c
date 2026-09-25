#include "driver/i2s_std.h"
#include <stdlib.h>
#include <string.h>

static int s_dummy_chan = 1;
static bool s_enabled = false;
static size_t s_bytes_written = 0;
static uint32_t s_write_count = 0;
static bool s_last_was_silence = false;

void test_i2s_reset(void)
{
    s_enabled = false;
    s_bytes_written = 0;
    s_write_count = 0;
    s_last_was_silence = false;
}

size_t test_i2s_get_bytes_written(void)
{
    return s_bytes_written;
}

uint32_t test_i2s_get_write_count(void)
{
    return s_write_count;
}

bool test_i2s_last_write_was_silence(void)
{
    return s_last_was_silence;
}

esp_err_t i2s_new_channel(const i2s_chan_config_t *chan_cfg, i2s_chan_handle_t *tx_handle, i2s_chan_handle_t *rx_handle)
{
    (void)chan_cfg;
    if (tx_handle != NULL) {
        *tx_handle = &s_dummy_chan;
    }
    if (rx_handle != NULL) {
        *rx_handle = NULL;
    }
    return ESP_OK;
}

esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t *std_cfg)
{
    (void)handle;
    (void)std_cfg;
    return ESP_OK;
}

esp_err_t i2s_channel_enable(i2s_chan_handle_t handle)
{
    (void)handle;
    s_enabled = true;
    return ESP_OK;
}

esp_err_t i2s_channel_disable(i2s_chan_handle_t handle)
{
    (void)handle;
    s_enabled = false;
    return ESP_OK;
}

esp_err_t i2s_channel_write(i2s_chan_handle_t handle, const void *src, size_t size, size_t *bytes_written, uint32_t timeout_ms)
{
    (void)handle;
    (void)timeout_ms;
    if (!s_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bytes_written != NULL) {
        *bytes_written = size;
    }
    s_bytes_written += size;
    s_write_count++;

    /* Check if src buffer is silence (all zeros) */
    s_last_was_silence = true;
    const uint8_t *p = (const uint8_t *)src;
    for (size_t i = 0; i < size; i++) {
        if (p[i] != 0) {
            s_last_was_silence = false;
            break;
        }
    }
    return ESP_OK;
}

esp_err_t i2s_del_channel(i2s_chan_handle_t handle)
{
    (void)handle;
    s_enabled = false;
    return ESP_OK;
}
