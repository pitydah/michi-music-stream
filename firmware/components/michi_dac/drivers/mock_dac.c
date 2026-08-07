/*
 * Mock DAC driver for tests/CI of later phases.
 *
 * Compiled always, but registered only when CONFIG_MICHI_DAC_MOCK is set
 * (Kconfig "Michi Music Stream Hardware" menu, default n). probe() succeeds
 * when MICHI_DAC_MOCK_PROBE_OK is set (default y), so CI without real DAC
 * hardware can exercise the manager state machine end to end. The mock never
 * touches any bus: bus_ctx is ignored.
 */

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"

#include "michi_dac_types.h"

static const char *TAG = "michi_dac_mock";

typedef struct {
    bool bound;
    uint8_t volume_0_100;
    bool muted;
    uint32_t sample_rate;
    uint8_t bit_depth;
    uint8_t channels;
} mock_ctx_t;

static mock_ctx_t s_ctx;

static esp_err_t mock_probe(const michi_dac_driver_t *drv, void *bus_ctx)
{
    (void)drv;
    (void)bus_ctx;
#ifdef CONFIG_MICHI_DAC_MOCK_PROBE_OK
    s_ctx.bound = true;
    ESP_LOGI(TAG, "probe ok (mock, CI only)");
    return ESP_OK;
#else
    ESP_LOGI(TAG, "probe failed (MICHI_DAC_MOCK_PROBE_OK disabled)");
    return ESP_ERR_NOT_FOUND;
#endif
}

static esp_err_t mock_init(const michi_dac_driver_t *drv, void *bus_ctx)
{
    (void)drv;
    (void)bus_ctx;
    s_ctx.volume_0_100 = 50;
    s_ctx.muted = false;
    ESP_LOGI(TAG, "init ok (mock)");
    return ESP_OK;
}

static esp_err_t mock_configure(const michi_dac_driver_t *drv, void *bus_ctx,
                                uint32_t sample_rate, uint8_t bit_depth,
                                uint8_t channels)
{
    (void)drv;
    (void)bus_ctx;
    s_ctx.sample_rate = sample_rate;
    s_ctx.bit_depth = bit_depth;
    s_ctx.channels = channels;
    ESP_LOGI(TAG, "configure ok (mock): %" PRIu32 " Hz, %u bit, %u ch",
             sample_rate, bit_depth, channels);
    return ESP_OK;
}

static esp_err_t mock_set_volume(const michi_dac_driver_t *drv, void *bus_ctx,
                                 uint8_t volume_0_100)
{
    (void)drv;
    (void)bus_ctx;
    s_ctx.volume_0_100 = volume_0_100 > 100 ? 100 : volume_0_100;
    return ESP_OK;
}

static esp_err_t mock_set_mute(const michi_dac_driver_t *drv, void *bus_ctx, bool mute)
{
    (void)drv;
    (void)bus_ctx;
    s_ctx.muted = mute;
    return ESP_OK;
}

static esp_err_t mock_get_status(const michi_dac_driver_t *drv, void *bus_ctx,
                                 michi_dac_status_t *status)
{
    (void)drv;
    (void)bus_ctx;
    memset(status, 0, sizeof(*status));
    status->present = s_ctx.bound;
    status->i2c_ok = true;
    status->pll_locked = true;
    status->clocks_ok = true;
    status->error_flag = false;
    status->volume_0_100 = s_ctx.volume_0_100;
    status->muted = s_ctx.muted;
    return ESP_OK;
}

static esp_err_t mock_shutdown(const michi_dac_driver_t *drv, void *bus_ctx)
{
    (void)drv;
    (void)bus_ctx;
    s_ctx.muted = true;
    s_ctx.bound = false;
    return ESP_OK;
}

/* Mock caps: a diagnostic device, never a product tier. */
const michi_dac_caps_t g_michi_dac_mock_caps = {
    .vendor = "Michi",
    .model = "Mock DAC",
    .board_profile = "mock",
    .max_sample_rate = 48000,
    .max_bit_depth = 16,
    .channels = 2,
    .snr_db = 0,
    .software_control = true,
    .hardware_volume = false,
    .hardware_mute = false,
    .differential_output = false,
    .headphone_output = false,
    .requires_mclk = false,
    .tier = MICHI_PRODUCT_DIAGNOSTIC, /* mock is never a product tier */
};

const michi_dac_driver_t g_michi_dac_mock = {
    .name = "mock",
    .board_profile = "mock",
    .self_detectable = true, /* probe outcome controlled by Kconfig */
    .template = &g_michi_dac_mock_caps,
    .ops = {
        .probe = mock_probe,
        .init = mock_init,
        .configure = mock_configure,
        .set_volume = mock_set_volume,
        .set_mute = mock_set_mute,
        .get_status = mock_get_status,
        .shutdown = mock_shutdown,
    },
};
