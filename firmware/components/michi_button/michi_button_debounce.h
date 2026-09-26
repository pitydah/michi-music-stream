#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Deterministic, hardware-agnostic debouncer for the physical pairing button
 * (PAIR-BTN-01).
 *
 * The debouncer is the SINGLE AUTHORITY for edge confirmation. It consumes
 * raw GPIO samples (fed by the polling task - never by the ISR) and a monotonic
 * timestamp, and emits exactly one event per stable transition. It knows
 * nothing about GPIO, FreeRTOS, the state machine or pairing: it is a pure
 * level/state machine so the SAME source is unit-tested host-side without
 * any hardware or RTOS plumbing (tests/host/test_michi_button_debounce.c).
 *
 * Model (MS-11-style single authority):
 *   raw_level       <- last sample fed in feed()
 *   candidate_level   <- the level being accumulated (what raw is right now)
 *   candidate_since_us <- when candidate_level was first observed at this value
 *   stable_level    <- the last CONFIRMED level
 *
 * A transition is confirmed ONLY when candidate_level == raw_level for a full
 * `debounce_ms` window AND candidate_level != stable_level. The ISR is no
 * longer consulted for release validity (PAIR-BTN-01 P0): a rebound that
 * survives polling but reverts before the debounce window is rejected by the
 * single stable-state authority, and a stable release is never aborted by a
 * later raw re-check.
 *
 * Active-low button: level 0 == pressed, level 1 == released/idle.
 */

typedef enum {
    MICHI_BTN_DEBOUNCE_NONE = 0,  /*!< No stable transition this sample */
    MICHI_BTN_DEBOUNCE_PRESS,     /*!< Stable transition to pressed (active-low 0) */
    MICHI_BTN_DEBOUNCE_RELEASE,   /*!< Stable transition to released (1) */
} michi_button_debounce_evt_t;

typedef struct {
    /* Configurable input */
    uint32_t debounce_ms;

    /* State (single authority) */
    int stable_level;          /* 1 = released/idle, 0 = pressed (confirmed) */
    int candidate_level;       /* level currently being accumulated */
    int64_t candidate_since_us;/* monotonic time when candidate_level began */
    int64_t stable_since_us;   /* monotonic time of the last stable confirmation */

    /* Diagnostics (best-effort, never consulted for decisions) */
    uint32_t samples_fed;      /* total feed() calls */
    uint32_t level_changes;    /* raw level transitions seen by feed() */
    uint32_t stable_presses;   /* confirmed PRESS events */
    uint32_t stable_releases;  /* confirmed RELEASE events */
} michi_button_debounce_t;

/**
 * @brief Initialize the debouncer.
 *
 * @param d           Debouncer state (zero-initialized first).
 * @param debounce_ms Stable-window duration (ms). The caller typically passes
 *                    CONFIG_MICHI_BUTTON_DEBOUNCE_MS but the unit is pure so
 *                    tests can drive any value.
 */
void michi_button_debounce_init(michi_button_debounce_t *d, uint32_t debounce_ms);

/**
 * @brief Feed one raw GPIO sample taken at @p now_us.
 *
 * @return The stable transition this sample completed (NONE in the common
 *         case where the level is still settling). At most one transition
 *         per call.
 */
michi_button_debounce_evt_t michi_button_debounce_feed(michi_button_debounce_t *d,
                                                       int raw_level,
                                                       int64_t now_us);

#ifdef __cplusplus
}
#endif
