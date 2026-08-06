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
 *    digital gain so the value is STILL applied - get() always reflects
 *    what the pipeline produces.
 *
 * The phase-12 volume handler MUST answer with michi_volume_get() (the
 * clamped, actually applied value), never with the request payload.
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
 *         hardware failures are logged and downgraded to digital gain.
 */
esp_err_t michi_volume_set(uint8_t v);

/**
 * @brief Get the REAL applied volume (0-100, always clamped).
 *
 * This is the value the API layer must report back.
 */
uint8_t michi_volume_get(void);

/**
 * @brief Apply digital gain to a PCM buffer (delegated by the audio output
 *        write path; no-op when the DAC hardware volume is active).
 *
 * @param buf       Sample buffer. For bit_depth 16: int16 samples. For
 *                  bit_depth 24: 3-byte packed samples (ESP32-S3 I2S
 *                  24-bit layout, full scale 0x7FFFFF) - NOT 32-bit
 *                  containers.
 * @param bytes     Byte count (16-bit: bytes/2 samples; 24-bit: bytes/3
 *                  samples).
 * @param bit_depth 16 or 24; anything else is a no-op (logged once).
 */
void michi_volume_apply(uint8_t *buf, size_t bytes, uint8_t bit_depth);

#ifdef __cplusplus
}
#endif
