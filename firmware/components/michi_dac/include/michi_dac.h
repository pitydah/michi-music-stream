#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "michi_dac_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Firmware-side DAC abstraction (phase 2: register + detect).
 *
 * Lifecycle: michi_dac_init() -> michi_dac_detect() -> michi_dac_start().
 * The full cycle init -> detect -> start -> shutdown -> detect -> start is
 * supported: shutdown() unbinds the driver and returns to NONE, so the next
 * detect() probes again.
 * All errors are propagated to the caller; nothing is faked.
 *
 * Detection model:
 * - If NVS key "dac_profile" (namespace "michi_dac", string, max 63 chars) is set,
 *   its value force-binds the matching driver (e.g. "pcm5122", "pcm5102a").
 * - Otherwise the registry is walked and every self_detectable driver is probed
 *   in order (pcm512x first, then mock when enabled).
 * - Detecting PCM5122 over I2C is stable, so it never writes the profile back.
 */

/**
 * @brief Source of a hardware board profile string (extension point).
 *
 * Registered via michi_dac_register_hw_id_source() and consulted during
 * detection when no NVS profile is set. Intended for boards that carry the
 * DAC identity in an EEPROM/ID chip (not implemented in phase 2 - no such
 * hardware exists yet); the callback just returns a board_profile string.
 *
 * @param[out] board_profile Buffer the profile is written into.
 * @param[in]  max_len       Size of the buffer.
 * @return ESP_OK with a non-empty profile to force-bind it;
 *         ESP_ERR_NOT_FOUND to fall through to autodetection;
 *         any other error propagates out of michi_dac_detect().
 */
typedef esp_err_t (*michi_dac_hw_id_fn)(char *board_profile, size_t max_len);

/**
 * @brief Initialize the DAC subsystem: static driver registry and NVS profile
 *        read. The I2C bus is created lazily, only when a driver that talks
 *        over I2C may be used (self-detectable probe or an I2C DAC profile);
 *        profile-only non-I2C DACs (PCM5102A) never need the bus.
 *
 * @return ESP_OK; errors (NVS, I2C bus) are propagated.
 */
esp_err_t michi_dac_init(void);

/**
 * @brief Detect and bind a DAC driver (probe or NVS force-bind).
 *
 * Idempotent once a driver is bound. An unknown NVS profile is logged and
 * falls back to autodetection. Real I2C bus failures are propagated;
 * a device that does not ACK or fails sanity yields ESP_ERR_NOT_FOUND.
 *
 * @return ESP_OK when a driver is bound, ESP_ERR_NOT_FOUND when nothing
 *         matches, other errors from the bus.
 */
esp_err_t michi_dac_detect(void);

/**
 * @brief Start the bound DAC: driver init() then configure().
 *
 * Requires the DETECTED state (a driver bound by michi_dac_detect()).
 *
 * @param sample_rate Sample rate in Hz (phase 2 validates 48000 only).
 * @param bit_depth   Bits per sample (16 or 24).
 * @param channels    Channel count (2).
 * @return ESP_OK when the DAC reports healthy clocks and reads back its
 *         configuration; ESP_ERR_INVALID_STATE when no driver is bound or
 *         the DAC cannot be initialized (e.g. no I2S clocks yet - see README);
 *         other errors are propagated.
 */
esp_err_t michi_dac_start(uint32_t sample_rate, uint8_t bit_depth, uint8_t channels);

/**
 * @brief Get the capability profile of the bound DAC. Single source of truth
 *        for the product profile (consumed in phase 3).
 *
 * @return Pointer to the current caps. When no DAC is bound, returns caps
 *         with model "" and detected=false.
 */
const michi_dac_caps_t *michi_dac_get_caps(void);

/**
 * @brief Read live status from the DAC (registers when available).
 *
 * @param status Output status.
 * @return ESP_OK, or ESP_ERR_INVALID_STATE when no DAC is bound.
 */
esp_err_t michi_dac_get_status(michi_dac_status_t *status);

/**
 * @brief Set hardware volume (0-100). Requires an initialized DAC.
 */
esp_err_t michi_dac_set_volume(uint8_t volume_0_100);

/**
 * @brief Set hardware mute. Requires an initialized DAC.
 */
esp_err_t michi_dac_set_mute(bool mute);

/**
 * @brief Mute and park the DAC cleanly, then UNBIND it: state returns to NONE
 *        and s_bound is released, so a later detect() re-probes the bus
 *        (init -> detect -> start -> shutdown -> detect -> start works).
 */
esp_err_t michi_dac_shutdown(void);

/**
 * @brief Register a hardware board-profile source (EEPROM ID extension).
 *
 * @param fn Callback, NULL clears the registration.
 */
void michi_dac_register_hw_id_source(michi_dac_hw_id_fn fn);

#ifdef __cplusplus
}
#endif
