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
 * (display phase 6, LED phase 7, button phase 8, network phase 9,
 * pairing phase 10, audio phase 11, API phase 12) reacts to the events
 * broadcast here; the
 * scattered booleans of the legacy firmware are replaced by this unique
 * source of truth.
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

    /* Phase 9 (network): emitted by michi_wifi; the four mapped events
     * below cover the unprovisioned -> connecting -> ready cycle and its
     * failure paths (map entries in michi_state.c). MICHI_EVENT_WIFI_*
     * events carry data 0. */
    MICHI_EVENT_WIFI_CONNECTED,    /*!< L2 link up (broadcast only, no mapping) */
    MICHI_EVENT_WIFI_DISCONNECTED, /*!< L2 link down; drives IDLE -> WIFI_CONNECTING (reconnect) */
    MICHI_EVENT_WIFI_PROVISIONED,  /*!< credentials received (BLE provisioning); drives UNPROVISIONED -> WIFI_CONNECTING */
    MICHI_EVENT_WIFI_PROV_FAILED,  /*!< provisioning failed or credentials erased; drives WIFI_CONNECTING -> UNPROVISIONED */
    MICHI_EVENT_NETWORK_READY,     /*!< IP obtained (STA_GOT_IP); drives WIFI_CONNECTING -> IDLE */

    /* Phase 8/10 (pairing): MICHI_EVENT_PAIRING_STARTED is emitted by the
     * physical button (michi_button) and drives IDLE/UNPROVISIONED ->
     * PAIRING; MICHI_EVENT_PAIRING_WINDOW_CLOSED is emitted by the
     * pairing flow (phase 10, michi_pairing) whenever the button-opened
     * window closes (expiry, successful pairing, attempt exhaustion,
     * explicit close) and drives PAIRING -> IDLE. */
    MICHI_EVENT_PAIRING_STARTED,   /*!< data: 0 */
    MICHI_EVENT_PAIRING_WINDOW_CLOSED, /*!< data: 0 */

    /* Phase 12 (sessions/API): emitted by the session layer (michi_session).
     * SESSION_STARTED is posted once per lifecycle step (negotiated ->
     * engine starting -> running) - the from-keyed map entries advance
     * IDLE -> SESSION_PENDING -> BUFFERING -> PLAYING. SESSION_CLOSED
     * ends the session from any session state (-> IDLE). PAUSED/RESUMED
     * switch between PLAYING and PAUSED (pause stops the engine and
     * retains the session state). */
    MICHI_EVENT_SESSION_STARTED,   /*!< data: 0 */
    MICHI_EVENT_SESSION_CLOSED,    /*!< data: 0 */
    MICHI_EVENT_SESSION_PAUSED,    /*!< data: 0 */
    MICHI_EVENT_SESSION_RESUMED,   /*!< data: 0 */

    /* Phase 13 (OTA update). */
    MICHI_EVENT_UPDATE_AVAILABLE,  /*!< data: 0 */
    MICHI_EVENT_UPDATE_STARTED,    /*!< data: 0 */
    MICHI_EVENT_UPDATE_DONE,       /*!< data: 0 */
    MICHI_EVENT_UPDATE_FAILED,     /*!< data: esp_err_t */

    /* Internal, not postable: posted ONLY by michi_state_request(); post()
     * and post_from_isr() reject it (and MICHI_EVENT_STATE_CHANGED) with
     * ESP_ERR_INVALID_ARG. Ids in the gap between the last domain event and
     * this marker are postable but broadcast-only (no transition mapping). */
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
 * @brief Snapshot of the LAST error captured by the FSM (phase 14
 *        diagnostics, michi_state_get_last_error).
 *
 * Captured by the FSM task (single writer, under the state mux) from:
 *  - every dispatched MICHI_EVENT_ERROR (data = the esp_err_t broadcast);
 *  - every dispatched MICHI_EVENT_UPDATE_FAILED (data = the esp_err_t,
 *    OTA uses its own event - captured by the same slot so the cause
 *    surfaces in diagnostics without a duplicated error broadcast);
 *  - a transition REQUEST to RECOVERABLE_ERROR/FATAL_ERROR, ONLY when no
 *    error event was ever captured or the last capture was itself a
 *    request (never overwrites a real error event: producers post the
 *    event before requesting the state, so the event wins).
 *
 * `recorded == false` means no error has been captured this boot.
 */
typedef struct {
    michi_event_id_t event; /*!< MICHI_EVENT_ERROR, MICHI_EVENT_UPDATE_FAILED */
    uint32_t data;          /*!< The esp_err_t carried by the event; 0 for a request-only capture */
    bool recorded;          /*!< false = no error captured yet this boot */
} michi_last_error_t;

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
 * Id contract:
 * - ids >= MICHI_EVENT_TRANSITION_REQUEST are rejected with
 *   ESP_ERR_INVALID_ARG (the transition-request event is internal to
 *   michi_state_request(), and the range past it is reserved);
 * - ids in the gap between the last declared domain event and
 *   MICHI_EVENT_TRANSITION_REQUEST are postable but have no mapping:
 *   broadcast-only (observers see them, no transition);
 * - MICHI_EVENT_STATE_CHANGED is broadcast-only FROM THE FSM: post() and
 *   post_from_isr() enforce the contract by rejecting it with
 *   ESP_ERR_INVALID_ARG - a posted STATE_CHANGED would be dispatched with
 *   a non-authoritative from/data and corrupt the observers' view.
 *
 * @param id   Event id (see contract above).
 * @param data Event payload.
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init; ESP_ERR_TIMEOUT when
 *         the queue is full (event dropped); ESP_ERR_INVALID_ARG for the
 *         reserved internal event ids (incl. MICHI_EVENT_STATE_CHANGED).
 */
esp_err_t michi_state_post(michi_event_id_t id, uint32_t data);

/**
 * @brief ISR-safe variant of michi_state_post() (e.g. pairing button, F8).
 *
 * Same id contract as post(); never blocks.
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

/**
 * @brief Copy the LAST error captured by the FSM (phase 14 diagnostics).
 *
 * Producers that broadcast MICHI_EVENT_ERROR (data = esp_err_t):
 *  - michi_wifi: retry chain exhausted (ESP_ERR_WIFI_NOT_CONNECT) before
 *    requesting RECOVERABLE_ERROR;
 *  - michi_audio: session engine self-end (socket/bind failure, pipeline
 *    write rejection - the session-ending failures).
 * OTA (michi_ota) uses its OWN event MICHI_EVENT_UPDATE_FAILED with the
 * same payload shape; it is captured by the same slot (no duplicated
 * error broadcast). See michi_last_error_t for the capture rules.
 *
 * @param out Output struct (must not be NULL).
 * @return ESP_OK; ESP_ERR_INVALID_ARG on NULL out; ESP_ERR_NOT_FOUND when
 *         no error has been captured this boot (out->recorded = false).
 */
esp_err_t michi_state_get_last_error(michi_last_error_t *out);

#ifdef __cplusplus
}
#endif
