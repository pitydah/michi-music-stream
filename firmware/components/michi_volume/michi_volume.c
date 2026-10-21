/*
 * Volume subsystem: 0-100 clamped, hardware (DAC) or digital (pipeline) gain.
 *
 * The reported numeric level is the clamped, configured gain (P0-12).
 * Mute is an independent PCM-silencing state: get() retains the configured
 * level while muted, and is_muted() reports the gate. A failed hardware
 * DAC write falls back to digital gain but physical output still needs
 * hardware verification in that degraded state.
 *
 * Threading contract: volume, DAC path and the independent mute gate are
 * atomic. The I2S consumer reads the latest state without a C data race.
 * Mute is applied as PCM silence even when the DAC uses hardware volume, so
 * unmute never needs to guess or reset the user's configured level.
 */

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"

#include "michi_dac.h"
#include "michi_volume.h"

#define TAG "michi_volume"

#define MICHI_VOLUME_MIN 0
#define MICHI_VOLUME_MAX 100

static _Atomic uint8_t s_volume = MICHI_VOLUME_MAX; /* user volume, unchanged by mute */
static _Atomic bool s_hw_active = false;           /* DAC hardware volume in use */
static _Atomic bool s_muted = false;               /* PCM silence gate */
static bool s_digital_warned = false;

/* H8: sole writer is the I2S consumer task; configuration is set before
 * starting that task. Q27 retains twelve fractional bits beyond Q15,
 * avoiding per-frame divides and heap allocation on the audio thread. */
#define MICHI_GAIN_ONE_Q15 32768
#define MICHI_GAIN_FRAC_BITS 12
static uint8_t s_pcm_channels = 1;
static uint32_t s_ramp_frames = 480; /* 10 ms at the v1-lite rate 48 kHz */
static int32_t s_gain_q27 = 0;
static int32_t s_target_q27 = 0;
static int32_t s_gain_step_q27 = 0;
static uint32_t s_frames_left = 0;

void michi_volume_reset_envelope(void)
{
    /* Only called before start or from the single I2S worker at resume. */
    s_gain_q27 = 0;
    s_target_q27 = 0;
    s_gain_step_q27 = 0;
    s_frames_left = 0;
}

esp_err_t michi_volume_configure_pcm(uint32_t sample_rate, uint8_t channels)
{
    /* Lifecycle: only after stop / before the consumer starts. */
    if (sample_rate < 8000 || sample_rate > 96000 ||
        (channels != 1 && channels != 2)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_pcm_channels = channels;
    s_ramp_frames = sample_rate / 100U; /* fixed 10-ms fade */
    michi_volume_reset_envelope(); /* clean session fade-in */
    return ESP_OK;
}

static int32_t gain_next_frame_q15(int32_t target_q15)
{
    const int32_t requested_q27 = target_q15 << MICHI_GAIN_FRAC_BITS;
    if (requested_q27 != s_target_q27) {
        s_target_q27 = requested_q27;
        s_frames_left = s_ramp_frames;
        s_gain_step_q27 = s_frames_left > 0
            ? (s_target_q27 - s_gain_q27) / (int32_t)s_frames_left : 0;
    }
    if (s_frames_left != 0) {
        if (--s_frames_left == 0) {
            s_gain_q27 = s_target_q27; /* exact convergence */
        } else {
            s_gain_q27 += s_gain_step_q27;
        }
    }
    return (s_gain_q27 + (1 << (MICHI_GAIN_FRAC_BITS - 1))) >> MICHI_GAIN_FRAC_BITS;
}

esp_err_t michi_volume_init(void)
{
    const michi_dac_caps_t *caps = michi_dac_get_caps();
    s_hw_active = caps->hardware_volume && caps->initialized;
    s_volume = MICHI_VOLUME_MAX;
    s_muted = false;
    s_digital_warned = false;
    /* Start-up fade is reset by configure_pcm() at each new output session. */
    s_gain_q27 = 0;
    s_target_q27 = 0;
    s_frames_left = 0;
    if (s_hw_active) {
        esp_err_t err = michi_dac_set_volume(s_volume);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "michi_dac_set_volume(%u) failed in init: %s - "
                          "falling back to digital gain",
                     s_volume, esp_err_to_name(err));
            s_hw_active = false;
        }
    }
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
    /* Muting does not mutate the user's volume setting. */
    return s_volume;
}

void michi_volume_set_muted(bool muted)
{
    /* Always gates PCM, independent of hardware-volume availability. */
    s_muted = muted;
}

bool michi_volume_is_muted(void)
{
    return s_muted;
}

void michi_volume_apply(uint8_t *buf, size_t bytes, uint8_t bit_depth)
{
    if (buf == NULL || bytes == 0) {
        return;
    }
    /* All channels of a PCM frame share exactly the same gain. A ramp
     * target change begins on the next frame without a step discontinuity.
     * The hardware-volume path keeps its DAC attenuation; only the PCM
     * mute gate and session fade are applied in that path. */
    const bool muted = s_muted;
    const uint8_t volume = s_volume;
    const bool hardware_path = s_hw_active;
    const int32_t target_q15 = muted ? 0 :
        (hardware_path ? MICHI_GAIN_ONE_Q15 :
         ((int32_t)volume * MICHI_GAIN_ONE_Q15) / 100);
    if (bit_depth != 16 && bit_depth != 24) {
        if (muted) {
            memset(buf, 0, bytes); /* fail-safe mute even for bad formats */
        } else if (!s_digital_warned) {
            s_digital_warned = true;
            ESP_LOGW(TAG, "apply: unsupported bit_depth=%u (no-op)", bit_depth);
        }
        return;
    }
    const size_t sample_bytes = (size_t)bit_depth / 8;
    const size_t frame_bytes = sample_bytes * s_pcm_channels;
    const size_t complete_bytes = bytes - (bytes % frame_bytes);
    /* Incomplete trailing PCM is not sound; never emit a torn sample. The
     * I2S writer normally rejects this case before samples enter the ring. */
    if (complete_bytes != bytes) {
        memset(buf + complete_bytes, 0, bytes - complete_bytes);
    }
    for (size_t offset = 0; offset < complete_bytes; offset += frame_bytes) {
        const int32_t gain_q15 = gain_next_frame_q15(target_q15);
        if (gain_q15 == MICHI_GAIN_ONE_Q15) {
            continue;
        }
        if (gain_q15 == 0) {
            memset(buf + offset, 0, frame_bytes);
            continue;
        }
        for (uint8_t channel = 0; channel < s_pcm_channels; channel++) {
            uint8_t *sample = buf + offset + sample_bytes * channel;
            if (bit_depth == 16) {
                int16_t value;
                memcpy(&value, sample, sizeof(value));
                const int32_t scaled = ((int32_t)value * gain_q15) / MICHI_GAIN_ONE_Q15;
                const int16_t out = (int16_t)scaled;
                memcpy(sample, &out, sizeof(out));
            } else {
                int32_t value = (int32_t)(((uint32_t)sample[2] << 16) |
                                          ((uint32_t)sample[1] << 8) | sample[0]);
                if ((value & 0x800000) != 0) {
                    value -= 0x1000000; /* signed 24-bit, no negative shift */
                }
                const int32_t scaled = (int32_t)(((int64_t)value * gain_q15) /
                                                 MICHI_GAIN_ONE_Q15);
                const uint32_t bits = (uint32_t)scaled;
                sample[0] = (uint8_t)bits;
                sample[1] = (uint8_t)(bits >> 8);
                sample[2] = (uint8_t)(bits >> 16);
            }
        }
    }
}
