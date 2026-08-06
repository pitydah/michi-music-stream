#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Global state machine + event bus (phase 5).
 *
 * The single coordinator of the universal firmware: one current state, one
 * FreeRTOS queue, one FSM task. Every subsystem that changes product behavior
 * (display phase 6, LED phase 7, network phase 9, audio phase 11, API
 * phase 12) reacts to the events broadcast here; the scattered booleans of
 * the legacy firmware are replaced by this unique source of truth.
 *
 * Contract:
 * - State changes ONLY happen through michi_state_request() (validated
 *   against the transition table) or through the event->transition mapping
 *   (michi_state.c). No other code writes the state.
 * - All observers are invoked SEQUENTIALLY from the FSM task, one event at
 *   a time (no concurrent dispatch, no reentrancy from the FSM itself).
 *   Observers MUST NOT block: no vTaskDelay, no blocking I/O, no
 *   esp_event_post_blocking. Keep them short - they run on the event path.
 * - michi_state_post()/post_from_isr() never block: a full queue drops the
 *   event and returns ESP_ERR_TIMEOUT (the FSM task must stay free to drain).
 */

/**
 * @brief Product states (the ONLY states the firmware can be in).
 */
typedef enum {
    MICHI_STATE_BOOTING = 0,     /*!< Start-up until boot events are processed */
    MICHI_STATE_SELF_TEST,       /*!< Running the board self-test */
    MICHI_STATE_UNPROVISIONED,   /*!< No network profile stored (network phase 9) */
    MICHI_STATE_PROVISIONING,    /*!< Provisioning flow active */
    MICHI_STATE_WIFI_CONNECTING, /*!< Connecting to the configured AP */
    MICHI_STATE_IDLE,            /*!< Steady state: ready for pairing/session */
    MICHI_STATE_PAIRING,         /*!< Pairing flow open (phase 10) */
    MICHI_STATE_SESSION_PENDING, /*!< Session negotiated, playback not started */
    MICHI_STATE_BUFFERING,       /*!< Pre-filling before playback */
    MICHI_STATE_PLAYING,         /*!< Audio flowing */
    MICHI_STATE_PAUSED,          /*!< Session active, playback suspended */
    MICHI_STATE_UPDATING,        /*!< OTA in progress (phase 13) */
    MICHI_STATE_RECOVERABLE_ERROR, /*!< Degraded but retryable */
    MICHI_STATE_FATAL_ERROR,     /*!< Terminal: cannot continue */
    MICHI_STATE_COUNT            /*!< Not a state: array size marker */
} michi_state_t;

/**
 * @brief Bus events.
 *
 * Mapped events drive state transitions (table in michi_state.c); unmapped
 * events (MICHI_EVENT_ERROR, MICHI_EVENT_STATE_CHANGED and the phase events
 * below) are only broadcast to observers - their phases add the mappings.
 */
typedef enum {
    MICHI_EVENT_BOOT_COMPLETE = 1, /*!< app_main after all boot-critical inits; drives BOOTING->SELF_TEST */
    MICHI_EVENT_SELF_TEST_DONE,    /*!< after the self-test; data: 1=ok, 0=degraded (informational: any data drives SELF_TEST->IDLE) */
    MICHI_EVENT_STATE_CHANGED,     /*!< internal broadcast on every transition: data=target, from=previous state */
    MICHI_EVENT_ERROR,             /*!< subsystem error: data=esp_err_t (broadcast only) */
    MICHI_EVENT_RECOVER,           /*!< retry after RECOVERABLE_ERROR; drives RECOVERABLE_ERROR->IDLE */

    /* Phase 9 (network): emitted by michi_wifi; mapping in phase 9. */
    MICHI_EVENT_WIFI_CONNECTED,    /*!< data: 0 */
    MICHI_EVENT_WIFI_DISCONNECTED, /*!< data: 0 */

    /* Phase 10 (pairing): emitted by the pairing flow. */
    MICHI_EVENT_PAIRING_STARTED,   /*!< data: 0 */
    MICHI_EVENT_PAIRING_CLOSED,    /*!< data: 0 */

    /* Phase 12 (sessions/API): emitted by the session layer. */
    MICHI_EVENT_SESSION_STARTED,   /*!< data: 0 */
    MICHI_EVENT_SESSION_CLOSED,    /*!< data: 0 */

    /* Phase 13 (OTA update). */
    MICHI_EVENT_UPDATE_AVAILABLE,  /*!< data: 0 */
    MICHI_EVENT_UPDATE_STARTED,    /*!< data: 0 */
    MICHI_EVENT_UPDATE_DONE,       /*!< data: 0 */
    MICHI_EVENT_UPDATE_FAILED,     /*!< data: esp_err_t */

    /* Internal, not postable: posted ONLY by michi_state_request(); post()
     * and post_from_isr() reject it (and any id past the enum) with
     * ESP_ERR_INVALID_ARG. */
    MICHI_EVENT_TRANSITION_REQUEST = 0x100,
} michi_event_id_t;

/**
 * @brief A bus event.
 *
 * @note The `from` field is populated by the FSM task at dispatch time;
 *       producers must not rely on the value they pass. It is
 *       authoritative in exactly one case: MICHI_EVENT_STATE_CHANGED,
 *       where it holds the PREVIOUS state id (data holds the new one). For
 *       every other event it holds the state current when the event was
 *       dispatched (context for observers: "event X while in state Y").
 */
typedef struct {
    michi_event_id_t id;   /*!< Event id */
    uint32_t data;         /*!< Event payload (see each event's doc) */
    uint32_t from;         /*!< Dispatch-time context state (see @note) */
} michi_event_t;

/**
 * @brief Observer callback, invoked from the FSM task.
 *
 * MUST NOT block (no vTaskDelay, no blocking I/O). May call
 * michi_state_get()/michi_state_name(); registration from inside an
 * observer is allowed but discouraged.
 */
typedef void (*michi_state_observer_fn)(const michi_event_t *ev);

/**
 * @brief Create the queue + FSM task and set the initial state (BOOTING).
 *
 * State transitions are logged by the FSM task; no observer slot is consumed
 * for logging. Safe to call once; repeated calls return ESP_OK (idempotent).
 * Init failures propagate (queue/task allocation).
 *
 * @return ESP_OK; ESP_ERR_NO_MEM if queue/task creation fails.
 */
esp_err_t michi_state_init(void);

/**
 * @brief Post an event from task context (never blocks).
 *
 * Broadcast to matching observers; if the event has a mapping it may also
 * drive a state transition (validated against the transition table).
 *
 * @param id   Event id (MICHI_EVENT_TRANSITION_REQUEST and ids past the enum
 *             are rejected).
 * @param data Event payload.
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init; ESP_ERR_TIMEOUT when
 *         the queue is full (event dropped); ESP_ERR_INVALID_ARG for the
 *         internal reserved event ids.
 */
esp_err_t michi_state_post(michi_event_id_t id, uint32_t data);

/**
 * @brief ISR-safe variant of michi_state_post() (e.g. pairing button, F8).
 *
 * @return Same as post(); never blocks.
 */
esp_err_t michi_state_post_from_isr(michi_event_id_t id, uint32_t data);

/**
 * @brief Request a state transition, validated against the transition table.
 *
 * Valid target from the CURRENT state: the request is queued and applied by
 * the FSM task (which re-validates; a concurrent request can win first) and
 * MICHI_EVENT_STATE_CHANGED is broadcast to all observers. Invalid target:
 * nothing happens, a warn is logged and ESP_ERR_INVALID_STATE is returned.
 *
 * @note ESP_OK means validated+queued, NOT applied: phase 9-13 producers
 *       MUST treat MICHI_EVENT_STATE_CHANGED as the acknowledgment, since a
 *       concurrent request or queue pressure can drop the queued transition.
 *
 * @param target Requested state.
 * @return ESP_OK (validated, queued - see note); ESP_ERR_INVALID_STATE (not
 *         allowed from the current state, or before init); ESP_ERR_INVALID_ARG
 *         (target out of range); ESP_ERR_TIMEOUT (queue full).
 */
esp_err_t michi_state_request(michi_state_t target);

/**
 * @brief Register an observer.
 *
 * @param filter Event id to observe, or 0 to receive ALL events. Filters are
 *        applied uniformly, including MICHI_EVENT_STATE_CHANGED: observers
 *        that react to every state change register with
 *        MICHI_EVENT_STATE_CHANGED (or 0).
 * @param fn    Callback (NULL -> ESP_ERR_INVALID_ARG).
 * @return ESP_OK; ESP_ERR_NO_MEM when the observer table is full
 *         (MICHI_STATE_MAX_OBSERVERS); duplicate (fn, filter) is a no-op
 *         returning ESP_OK. May be called before init().
 */
esp_err_t michi_state_register_observer(michi_event_id_t filter,
                                        michi_state_observer_fn fn);

/**
 * @brief Get the current state (read from any task).
 */
michi_state_t michi_state_get(void);

/**
 * @brief Human-readable state name: "BOOTING"..."FATAL_ERROR".
 *
 * @return Static string; "UNKNOWN" for out-of-range values; never NULL.
 */
const char *michi_state_name(michi_state_t s);

#ifdef __cplusplus
}
#endif
