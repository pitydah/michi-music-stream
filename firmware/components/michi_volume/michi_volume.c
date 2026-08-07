/*
 * Volume subsystem: 0-100 clamped, hardware (DAC) or digital (pipeline) gain.
 *
 * The reported value is ALWAYS the applied value (P0-12): the phase-12 API
 * handler answers with michi_volume_get(), and get() only ever contains the
 * clamped value that set() stored and that apply()/the DAC will produce.
 *
 * Threading contract: s_volume and s_hw_active are written by the API task
 * (michi_volume_init/set, single writer) and read by the I2S task
 * (michi_volume_apply); volatile for cross-core visibility.
 */

#include <string.h>

#include "esp_log.h"

#include "michi_dac.h"
#include "michi_volume.h"

#define TAG "michi_volume"

#define MICHI_VOLUME_MIN 0
#define MICHI_VOLUME_MAX 100

static volatile uint8_t s_volume = MICHI_VOLUME_MAX; /* legacy default: full scale */
static volatile bool s_hw_active = false;            /* DAC hardware volume in use */
static bool s_digital_warned = false;

esp_err_t michi_volume_init(void)
{
    const michi_dac_caps_t *caps = michi_dac_get_caps();
    s_hw_active = caps->hardware_volume && caps->initialized;
    s_volume = MICHI_VOLUME_MAX;
    ESP_LOGI(TAG, "volume path=%s (dac hw volume=%s initialized=%s)",
             s_hw_active ? "hardware" : "digital",
             caps->hardware_volume ? "yes" : "no",
             caps->initialized ? "yes" : "no");
    return ESP_OK;
}

esp_err_t michi_volume_set(uint8_t v)
{
    /* v is uint8_t: the lower clamp is implied (0..255); only the upper
     * bound needs clamping (no -Wtype-limits on the unsigned floor). */
    if (v > MICHI_VOLUME_MAX) {
        v = MICHI_VOLUME_MAX;
    }
    s_volume = v;

    if (s_hw_active) {
        esp_err_t err = michi_dac_set_volume(v);
        if (err != ESP_OK) {
            /* The value must still be applied: fall back to digital gain.
             * get() keeps reporting s_volume, which the pipeline now
             * produces. */
            ESP_LOGW(TAG, "michi_dac_set_volume(%u) failed: %s - "
                          "falling back to digital gain",
                     v, esp_err_to_name(err));
            s_hw_active = false;
        }
    }
    ESP_LOGI(TAG, "volume=%u path=%s", s_volume, s_hw_active ? "hardware" : "digital");
    return ESP_OK;
}

uint8_t michi_volume_get(void)
{
    return s_volume;
}

void michi_volume_apply(uint8_t *buf, size_t bytes, uint8_t bit_depth)
{
    if (buf == NULL || bytes == 0) {
        return;
    }
    if (s_hw_active || s_volume >= MICHI_VOLUME_MAX) {
        return;
    }
    if (s_volume <= MICHI_VOLUME_MIN) {
        memset(buf, 0, bytes);
        return;
    }
    if (bit_depth == 16) {
        /* Q15 gain: factor = v/100 with 15 fractional bits. The product
         * (|in| <= 32767, factor <= 32768) fits in int32, so the shift
         * cannot overflow: no saturation needed. */
        const int32_t factor = ((int32_t)s_volume * 32768) / 100;
        int16_t *p = (int16_t *)buf;
        const size_t samples = bytes / 2;
        for (size_t i = 0; i < samples; i++) {
            /* Unsigned view before the shift (same pattern as the 24-bit
             * path): right-shifting a negative value is implementation-
             * defined in C11 6.5.7p5, not UB - cppcheck flags it as
             * shiftNegativeLHS. Truncating the logical shift to int16_t
             * produces the same bits as the arithmetic shift. */
            const uint32_t us = (uint32_t)((int32_t)p[i] * factor);
            p[i] = (int16_t)(us >> 15);
        }
    } else if (bit_depth == 24) {
        /* 24-bit samples are 3-byte packed, little-endian with byte[2] as
         * the MSB (ESP32-S3 I2S 24-bit layout, i2s_get_buf_size = 3): NOT
         * int32 containers, so no int32_t* casts here. Q23 gain with
         * saturation to the 24-bit full scale. */
        const int64_t factor = ((int64_t)s_volume * (1LL << 23)) / 100;
        for (size_t i = 0; i + 3 <= bytes; i += 3) {
            int32_t val = (int32_t)((buf[i + 2] << 16) |
                                    (buf[i + 1] << 8) | buf[i]);
            val = val | -(val & 0x800000); /* sign-extend bit 23 */
            int64_t s = ((int64_t)val * factor) >> 23;
            if (s > 0x7FFFFF) {
                s = 0x7FFFFF;
            } else if (s < -0x800000) {
                s = -0x800000;
            }
            /* Unsigned view before shifting: right-shifting a negative
             * value is implementation-defined in C11 6.5.7p5, not UB
             * (cppcheck flags it as shiftNegativeLHS); the bytes of a
             * two's-complement negative are identical when the value is
             * reinterpreted as unsigned. */
            const uint64_t us = (uint64_t)s;
            buf[i]     = (uint8_t)(us & 0xFF);
            buf[i + 1] = (uint8_t)((us >> 8) & 0xFF);
            buf[i + 2] = (uint8_t)((us >> 16) & 0xFF);
        }
    } else if (!s_digital_warned) {
        s_digital_warned = true;
        ESP_LOGW(TAG, "apply: unsupported bit_depth=%u (no-op), "
                      "supported: 16, 24", bit_depth);
    }
}
