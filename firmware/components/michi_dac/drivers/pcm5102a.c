/*
 * PCM5102A I2S DAC driver.
 *
 * The PCM5102A is a pure I2S DAC: no I2C/SPI control interface at all, so
 * there is nothing to probe and nothing to configure over a bus. It can only
 * be bound through the explicit NVS profile "pcm5102a" (an intentional user
 * assertion on the board), never by autodetection. probe() therefore returns
 * ESP_ERR_NOT_SUPPORTED - honest, not a fake detection.
 *
 * All control functions that would need registers return ESP_ERR_NOT_SUPPORTED;
 * volume/mute depend on external hardware (analog pot, headphone amp), which
 * the firmware cannot drive.
 */

#include <string.h>

#include "esp_log.h"

#include "michi_dac_types.h"

static const char *TAG = "michi_dac_pcm5102a";

static esp_err_t pcm5102a_probe(const michi_dac_driver_t *drv, void *bus_ctx)
{
    (void)drv;
    (void)bus_ctx;
    return ESP_ERR_NOT_SUPPORTED; /* no control bus exists on this DAC */
}

static esp_err_t pcm5102a_init(const michi_dac_driver_t *drv, void *bus_ctx)
{
    (void)drv;
    (void)bus_ctx;
    /* No registers: I2S in, analog out. Nothing to do, nothing to fake. */
    ESP_LOGI(TAG, "init ok: I2S DAC, no control interface");
    return ESP_OK;
}

static esp_err_t pcm5102a_configure(const michi_dac_driver_t *drv, void *bus_ctx,
                                    uint32_t sample_rate, uint8_t bit_depth,
                                    uint8_t channels)
{
    (void)drv;
    (void)bus_ctx;
    if (sample_rate != 48000 || channels != 2 ||
        (bit_depth != 16 && bit_depth != 24)) {
        ESP_LOGW(TAG, "configure unsupported: %" PRIu32 " Hz / %u bit / %u ch "
                 "(phase 2 validates 48000 Hz, 16|24 bit, 2 ch)",
                 sample_rate, bit_depth, channels);
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* The DAC clocks from BCLK/LRCK (no MCLK pin on PCM5102A). */
    ESP_LOGI(TAG, "configure ok: %" PRIu32 " Hz, %u bit, %u ch (no registers to set)",
             sample_rate, bit_depth, channels);
    return ESP_OK;
}

static esp_err_t pcm5102a_set_volume(const michi_dac_driver_t *drv, void *bus_ctx,
                                     uint8_t volume_0_100)
{
    (void)drv;
    (void)bus_ctx;
    (void)volume_0_100;
    return ESP_ERR_NOT_SUPPORTED; /* no register control on this DAC */
}

static esp_err_t pcm5102a_set_mute(const michi_dac_driver_t *drv, void *bus_ctx, bool mute)
{
    (void)drv;
    (void)bus_ctx;
    (void)mute;
    return ESP_ERR_NOT_SUPPORTED; /* no register control on this DAC */
}

static esp_err_t pcm5102a_get_status(const michi_dac_driver_t *drv, void *bus_ctx,
                                     michi_dac_status_t *status)
{
    (void)drv;
    (void)bus_ctx;
    memset(status, 0, sizeof(*status));
    status->present = true;   /* bound via explicit profile (profile-asserted, unverified) */
    status->i2c_ok = false;   /* no I2C bus on this DAC */
    /* pll_locked/clocks_ok are unknown: there is no status interface. */
    return ESP_OK;
}

static esp_err_t pcm5102a_shutdown(const michi_dac_driver_t *drv, void *bus_ctx)
{
    (void)drv;
    (void)bus_ctx;
    ESP_LOGI(TAG, "shutdown: nothing to do (no registers)");
    return ESP_OK;
}

/* Caps: silicon numbers from the PCM5102A datasheet (up to 192 kHz / 24-bit /
 * 2 ch, SNR 112 dB typ, single-ended outputs). board_verified is ALWAYS false
 * for this DAC: it can only be bound by NVS profile, and a profile is a user
 * assertion (profile-asserted, unverified) - there is no register or bus to
 * prove the part is present. tier is STANDARD always: this DAC is the
 * Standard product variant, never classified HiFi even when initialized (no
 * register control, no PLL verification possible). */
const michi_dac_caps_t g_michi_dac_pcm5102a_caps = {
    .vendor = "TI",
    .model = "PCM5102A",
    .board_profile = "pcm5102a",
    .max_sample_rate = 192000, /* silicon limit; validated at 48 kHz in phase 2 */
    .max_bit_depth = 24,
    .channels = 2,
    .snr_db = 112,
    .software_control = false,
    .hardware_volume = false,
    .hardware_mute = false,
    .differential_output = false,
    .headphone_output = false,
    .requires_mclk = false, /* clocks from BCLK/LRCK */
    .tier = MICHI_PRODUCT_STANDARD, /* always: never HiFi, even when initialized */
};

const michi_dac_driver_t g_michi_dac_pcm5102a = {
    .name = "pcm5102a",
    .board_profile = "pcm5102a",
    .self_detectable = false, /* bound ONLY via explicit NVS profile */
    .template = &g_michi_dac_pcm5102a_caps,
    .ops = {
        .probe = pcm5102a_probe,
        .init = pcm5102a_init,
        .configure = pcm5102a_configure,
        .set_volume = pcm5102a_set_volume,
        .set_mute = pcm5102a_set_mute,
        .get_status = pcm5102a_get_status,
        .shutdown = pcm5102a_shutdown,
    },
};
