#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SK6812 status LEDs (phase 7): the three status LEDs of the M5Stack
 *        U003, driven by one RMT-led_strip channel.
 *
 * Architecture (see firmware/README.md, LED section):
 * - The michi_state observer ONLY updates a volatile pattern (short
 *   portMUX critical section) - it NEVER touches the strip: RMT writes are
 *   slow and the observer runs on the FSM task (MUST-NOT-block contract,
 *   phase 5).
 * - The animation task (50 ms tick) owns the strip: it reads the current
 *   pattern, computes the three pixel colors from a precomputed sine LUT
 *   and calls led_strip_set_pixel + led_strip_refresh per tick. The
 *   refresh is synchronous (~0.25 ms per tick) but bounded and confined
 *   to the animation task; the observer (FSM task) never blocks.
 * - Every color is scaled by MICHI_LED_MAX_BRIGHTNESS_PCT/100 in software
 *   before reaching the strip (brightness cap, never configured in
 *   hardware).
 *
 * Scope restriction: ONLY the three SK6812 status LEDs. The cat-contour
 * strip is NOT implemented (no extra GPIO, no second channel) - the only
 * mention is lighting_cat_contour=false in the product profile.
 */

/**
 * @brief Initialize the LED subsystem: create the led_strip device (RMT,
 *        one channel, GPIO + count from Kconfig), register the state
 *        observer and start the animation task.
 *
 * Must be called after michi_state_init(). Safe to call once; repeated
 * calls return ESP_OK (idempotent). On failure app_main continues degraded:
 * no status LEDs, everything else keeps working.
 *
 * @return ESP_OK; raw driver errors from led_strip_new_rmt_device are
 *         propagated unchanged (e.g. ESP_ERR_INVALID_ARG for an invalid
 *         GPIO, ESP_ERR_NO_MEM for the RMT channel/strip allocation or
 *         the task creation).
 */
esp_err_t michi_led_init(void);

/**
 * @brief Shut the LEDs down: cooperative task stop (flag + notification +
 *        join with timeout), LEDs cleared by the task itself, strip
 *        released.
 *
 * Idempotent: a second call when the subsystem is already off returns
 * ESP_OK (the first call left it fully shut down). Safe to call from any
 * task; the caller must not be the animation task. If the task does not
 * stop within the join timeout the strip is NOT deleted (the task may
 * still be running) - a warn is logged and the strip is leaked rather
 * than freed under a live task.
 *
 * @return ESP_OK; ESP_ERR_TIMEOUT if the animation task did not stop
 *         within the join timeout (strip leaked); ESP_ERR_INVALID_STATE
 *         only if called from the animation task itself.
 */
esp_err_t michi_led_shutdown(void);

#ifdef __cplusplus
}
#endif
