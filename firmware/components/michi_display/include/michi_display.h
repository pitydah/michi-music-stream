#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Integrated display subsystem (phase 6): state-driven screens on the
 *        board panel, rendered by a dedicated task.
 *
 * Architecture (see firmware/README.md, Display section):
 * - The michi_state observer ONLY enqueues render requests (timeout 0, drop
 *   + warn when full) - it MUST NOT block and never touches the
 *   framebuffer (observer contract, phase 5).
 * - The render task owns the framebuffer/flush (michi_board is not
 *   thread-safe for the display), coalesces pending requests and draws the
 *   screen for the CURRENT state (michi_state_get()).
 * - BOOTING and SELF_TEST are covered by the BSP boot screen rendered by
 *   app_main; this subsystem skips them (documented division).
 *
 * Producer contract: all producers (the observer, the session layer, the
 * volume/misc callers) MUST run in task context - the render queue uses
 * xQueueSend, never the FromISR variant. ISR producers must defer through
 * a task (e.g. post an event on the state bus).
 */

/*!< Max chars copied per field by michi_display_update_now_playing() */
#define MICHI_DISPLAY_SOURCE_MAX 32
#define MICHI_DISPLAY_TITLE_MAX 64
#define MICHI_DISPLAY_ARTIST_MAX 48

/**
 * @brief Initialize the display subsystem: register the state observer and
 *        create the render task with its own queue.
 *
 * Must be called after michi_state_init(). Safe to call once; repeated
 * calls return ESP_OK (idempotent). On failure app_main continues degraded:
 * no dynamic screens, the BSP boot screen still shows.
 *
 * @return ESP_OK; ESP_ERR_NO_MEM if the queue/task allocation or the
 *         observer registration fails (observer table full).
 */
esp_err_t michi_display_init(void);

/**
 * @brief Update the now-playing metadata (copied into internal buffers,
 *        truncated to MICHI_DISPLAY_SOURCE_MAX/TITLE_MAX/ARTIST_MAX) and
 *        request a re-render of the current state.
 *
 * Called by the session layer (phase 12) on track changes, from TASK
 * context only (the render queue is not ISR-safe). Never blocks: a full
 * render queue drops the request (warn logged) and sets a pending flag
 * that makes the task re-render once after the drain; the next state
 * change re-renders anyway.
 *
 * @param source Source label; NULL/empty renders as "--".
 * @param title  Track title.
 * @param artist Artist.
 * @return ESP_OK; ESP_ERR_INVALID_STATE if the subsystem is not initialized
 *         (nothing was updated).
 */
esp_err_t michi_display_update_now_playing(const char *source,
                                           const char *title,
                                           const char *artist);

/**
 * @brief Request a re-render of the current state (e.g. volume changed).
 *
 * TASK context only (the render queue is not ISR-safe). Never blocks: a
 * full render queue drops the request (warn logged) and sets a pending
 * flag that makes the task re-render once after the drain.
 */
void michi_display_request_redraw(void);

#ifdef __cplusplus
}
#endif
