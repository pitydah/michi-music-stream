#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Volume subsystem (phase 4: P0-12 - the reported value is
 *        ALWAYS the value actually applied).
 *
 * Model: 0-100, clamped at the API boundary. Two application paths:
 *  - Hardware volume: when the bound DAC has hardware volume and is
 *    initialized (michi_dac caps), michi_volume_set() writes it through
 *    michi_dac_set_volume() and michi_volume_apply() becomes a no-op.
 *  - Digital gain: otherwise the gain is applied in the audio pipeline
 *    (michi_volume_apply(), called by michi_audio_output on the write
 *    path). If a hardware volume write fails, the subsystem falls back to
 *    digital gain. An unknown residual hardware attenuation may remain
 *    after a failed DAC write; physical output cannot be certified by get().
 *
 * The phase-12 volume handler reports the configured volume using
 * michi_volume_get(); independent output muting never changes that level.
 * Mute gates PCM toward zero over a bounded 10-ms fade regardless of the hardware DAC path.
 */

/**
 * @brief Initialize the volume subsystem: resolve hardware vs digital path
 *        from the current DAC caps.
 *
 * @return ESP_OK.
 */
esp_err_t michi_volume_init(void);

/**
 * @brief Set volume, clamped to 0-100.
 *
 * Hardware path: michi_dac_set_volume() when the DAC has hardware volume
 * and is initialized. On a hardware write failure the error is logged and
 * the subsystem falls back to digital gain - the requested (clamped) value
 * is still applied, so get() and the pipeline stay consistent.
 *
 * @param v Volume 0-100 (anything outside is clamped).
 * @return ESP_OK when the clamped value was applied (hardware or digital);
 *         ESP_ERR_INVALID_ARG is not used (clamping makes it unreachable);
 *         hardware failures are logged and downgraded to digital gain,
 *         but require physical verification of the resulting output level.
 */
esp_err_t michi_volume_set(uint8_t v);

/**
 * @brief Get the configured volume (0-100, clamped).
 *
 * Mute state is orthogonal; this does not return zero just because
 * michi_volume_is_muted() is true.
 */
uint8_t michi_volume_get(void);

/**
 * @brief Set or clear the independent PCM mute gate.
 *
 * Does not replace or alter the configured volume. Muting is enforced on
 * PCM even if the DAC has hardware-volume support. Unmuting restores the
 * existing level; setting the volume while muted updates that level for
 * later unmute. Initialized to unmuted by michi_volume_init().
 */
void michi_volume_set_muted(bool muted);

/** @brief Whether PCM samples are currently gated to silence. */
bool michi_volume_is_muted(void);

/**
 * @brief Apply digital gain to a PCM buffer (delegated by the audio output
 *        write path; the mute gate overrides both hardware and digital gain).
 *
 * @param buf       Sample buffer. For bit_depth 16: int16 samples. For
 *                  bit_depth 24: 3-byte packed samples (ESP32-S3 I2S
 *                  24-bit layout, full scale 0x7FFFFF) - NOT 32-bit
 *                  containers.
 * @param bytes     Byte count (16-bit: bytes/2 samples; 24-bit: bytes/3
 *                  samples).
 * @param bit_depth 16 or 24; anything else is a no-op (logged once).
 */
/**
 * @brief Configure the consumer's interleaved PCM layout and reset its ramp.
 * Must run before the I2S consumer task starts; never during playback.
 * A 10 ms fade is calculated from the real stream sample rate (8-96 kHz).
 */
esp_err_t michi_volume_configure_pcm(uint32_t sample_rate, uint8_t channels);

/** Reset gain to silence for fade-in after START or RESUME.
 * Must be called only before the I2S task starts, or by that task itself.
 */
void michi_volume_reset_envelope(void);

/**
 * @brief Apply per-frame, 10-ms linear gain ramps across chunk boundaries.
 * The independently reported mute state becomes acoustically silent at
 * the end of the fade; hardware DAC volume changes remain DAC-controlled.
 * Caller is the sole I2S consumer task (no concurrent apply calls).
 */
void michi_volume_apply(uint8_t *buf, size_t bytes, uint8_t bit_depth);

#ifdef __cplusplus
}
#endif
