#include "driver/i2c_master.h"
#include <string.h>

static bool s_bus_created = false;
static esp_err_t s_probe_results[128];

void test_i2c_reset(void)
{
    s_bus_created = false;
    for (int i = 0; i < 128; i++) {
        s_probe_results[i] = ESP_ERR_NOT_FOUND;
    }
}

bool test_i2c_bus_created(void)
{
    return s_bus_created;
}

void test_i2c_set_probe_result(uint16_t address, esp_err_t result)
{
    if (address < 128) {
        s_probe_results[address] = result;
    }
}

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *bus_config, i2c_master_bus_handle_t *ret_bus_handle)
{
    (void)bus_config;
    s_bus_created = true;
    if (ret_bus_handle) {
        *ret_bus_handle = (void *)0x12C0;
    }
    return ESP_OK;
}

esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus_handle)
{
    (void)bus_handle;
    s_bus_created = false;
    return ESP_OK;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus_handle, uint16_t address, int xfer_timeout_ms)
{
    (void)bus_handle;
    (void)xfer_timeout_ms;
    if (bus_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (address < 128) {
        return s_probe_results[address];
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus_handle, const i2c_device_config_t *dev_config, i2c_master_dev_handle_t *ret_handle)
{
    (void)bus_handle;
    (void)dev_config;
    if (ret_handle) {
        *ret_handle = (void *)0x12CD;
    }
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t i2c_dev, const uint8_t *write_buffer, size_t write_size, int xfer_timeout_ms)
{
    (void)i2c_dev;
    (void)write_buffer;
    (void)write_size;
    (void)xfer_timeout_ms;
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t i2c_dev, const uint8_t *write_buffer, size_t write_size, uint8_t *read_buffer, size_t read_size, int xfer_timeout_ms)
{
    (void)i2c_dev;
    (void)write_buffer;
    (void)write_size;
    (void)xfer_timeout_ms;
    if (read_buffer && read_size > 0) {
        memset(read_buffer, 0, read_size);
    }
    return ESP_OK;
}
