#include "driver/i2s_std.h"
#include <stdlib.h>
#include <string.h>

#include <pthread.h>
#include <unistd.h>

static int s_dummy_chan = 1;
static bool s_enabled = false;
static size_t s_bytes_written = 0;
static uint32_t s_write_count = 0;
static bool s_last_was_silence = false;
static esp_err_t s_injected_write_err = ESP_OK;
static esp_err_t s_injected_new_chan_err = ESP_OK;
static esp_err_t s_injected_init_std_err = ESP_OK;
static uint32_t s_write_delay_ms = 0;
static pthread_t s_writer_thread = 0;
static bool s_has_writer_thread = false;
static bool s_multiple_writers = false;
static pthread_mutex_t s_i2s_shim_mux = PTHREAD_MUTEX_INITIALIZER;

#define CAPTURE_BUF_SIZE 65536
static uint8_t s_captured_data[CAPTURE_BUF_SIZE];
static size_t s_captured_size = 0;

void test_i2s_reset(void)
{
    pthread_mutex_lock(&s_i2s_shim_mux);
    s_enabled = false;
    s_bytes_written = 0;
    s_write_count = 0;
    s_last_was_silence = false;
    s_injected_write_err = ESP_OK;
    s_injected_new_chan_err = ESP_OK;
    s_injected_init_std_err = ESP_OK;
    s_write_delay_ms = 0;
    s_writer_thread = 0;
    s_has_writer_thread = false;
    s_multiple_writers = false;
    s_captured_size = 0;
    pthread_mutex_unlock(&s_i2s_shim_mux);
}

void test_i2s_clear_captured_data(void)
{
    pthread_mutex_lock(&s_i2s_shim_mux);
    s_captured_size = 0;
    pthread_mutex_unlock(&s_i2s_shim_mux);
}

void test_i2s_set_new_channel_fail(esp_err_t err)
{
    pthread_mutex_lock(&s_i2s_shim_mux);
    s_injected_new_chan_err = err;
    pthread_mutex_unlock(&s_i2s_shim_mux);
}

void test_i2s_set_init_std_mode_fail(esp_err_t err)
{
    pthread_mutex_lock(&s_i2s_shim_mux);
    s_injected_init_std_err = err;
    pthread_mutex_unlock(&s_i2s_shim_mux);
}

void test_i2s_set_write_fail(esp_err_t err)
{
    pthread_mutex_lock(&s_i2s_shim_mux);
    s_injected_write_err = err;
    pthread_mutex_unlock(&s_i2s_shim_mux);
}

void test_i2s_set_write_delay_ms(uint32_t ms)
{
    pthread_mutex_lock(&s_i2s_shim_mux);
    s_write_delay_ms = ms;
    pthread_mutex_unlock(&s_i2s_shim_mux);
}

bool test_i2s_multiple_writers_detected(void)
{
    pthread_mutex_lock(&s_i2s_shim_mux);
    bool m = s_multiple_writers;
    pthread_mutex_unlock(&s_i2s_shim_mux);
    return m;
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
    pthread_mutex_lock(&s_i2s_shim_mux);
    esp_err_t err = s_injected_new_chan_err;
    s_injected_new_chan_err = ESP_OK;
    pthread_mutex_unlock(&s_i2s_shim_mux);
    if (err != ESP_OK) {
        return err;
    }
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
    pthread_mutex_lock(&s_i2s_shim_mux);
    esp_err_t err = s_injected_init_std_err;
    s_injected_init_std_err = ESP_OK;
    pthread_mutex_unlock(&s_i2s_shim_mux);
    return err;
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

    pthread_mutex_lock(&s_i2s_shim_mux);
    if (!s_enabled) {
        pthread_mutex_unlock(&s_i2s_shim_mux);
        return ESP_ERR_INVALID_STATE;
    }

    pthread_t self = pthread_self();
    if (!s_has_writer_thread) {
        s_writer_thread = self;
        s_has_writer_thread = true;
    } else if (!pthread_equal(s_writer_thread, self)) {
        s_multiple_writers = true;
    }

    uint32_t delay_ms = s_write_delay_ms;
    esp_err_t injected_err = s_injected_write_err;
    if (injected_err != ESP_OK) {
        s_injected_write_err = ESP_OK; /* One-shot injection */
        pthread_mutex_unlock(&s_i2s_shim_mux);
        return injected_err;
    }
    pthread_mutex_unlock(&s_i2s_shim_mux);

    if (delay_ms > 0) {
        usleep(delay_ms * 1000);
    }

    pthread_mutex_lock(&s_i2s_shim_mux);
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

    /* Record into capture buffer */
    if (s_captured_size < CAPTURE_BUF_SIZE) {
        size_t to_copy = size;
        if (s_captured_size + to_copy > CAPTURE_BUF_SIZE) {
            to_copy = CAPTURE_BUF_SIZE - s_captured_size;
        }
        memcpy(s_captured_data + s_captured_size, src, to_copy);
        s_captured_size += to_copy;
    }

    pthread_mutex_unlock(&s_i2s_shim_mux);
    return ESP_OK;
}

esp_err_t i2s_del_channel(i2s_chan_handle_t handle)
{
    (void)handle;
    s_enabled = false;
    return ESP_OK;
}

size_t test_i2s_get_captured_bytes(uint8_t *dst, size_t max_len)
{
    pthread_mutex_lock(&s_i2s_shim_mux);
    size_t n = s_captured_size;
    if (n > max_len) {
        n = max_len;
    }
    if (dst != NULL && n > 0) {
        memcpy(dst, s_captured_data, n);
    }
    pthread_mutex_unlock(&s_i2s_shim_mux);
    return n;
}

bool test_i2s_contains_byte(uint8_t byte)
{
    pthread_mutex_lock(&s_i2s_shim_mux);
    bool found = false;
    for (size_t i = 0; i < s_captured_size; i++) {
        if (s_captured_data[i] == byte) {
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&s_i2s_shim_mux);
    return found;
}
