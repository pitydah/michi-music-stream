/* Fake michi_volume for host-side tests: implements the REAL
 * michi_volume.h against a plain variable (the digital path; clamping
 * mirrors the firmware API). TEST-ONLY: never compiled into firmware. */

#include "michi_volume.h"

static uint8_t s_volume;
static bool s_muted;

void test_volume_reset(void)
{
    s_volume = 0;
    s_muted = false;
}

uint8_t test_volume_value(void)
{
    return s_volume;
}

esp_err_t michi_volume_set(uint8_t v)
{
    s_volume = v > 100 ? 100 : v;
    return ESP_OK;
}

uint8_t michi_volume_get(void)
{
    return s_volume;
}

void michi_volume_apply(uint8_t *buf, size_t bytes, uint8_t bit_depth)
{
    (void)buf;
    (void)bytes;
    (void)bit_depth;
}

/* Host shim for H8's new audio-consumer lifecycle functions. */
esp_err_t michi_volume_configure_pcm(uint32_t sample_rate, uint8_t channels)
{
    if (sample_rate < 8000 || sample_rate > 96000 ||
        (channels != 1 && channels != 2)) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

void michi_volume_reset_envelope(void) {}

void michi_volume_set_muted(bool muted)
{
    s_muted = muted;
}

bool michi_volume_is_muted(void)
{
    return s_muted;
}
