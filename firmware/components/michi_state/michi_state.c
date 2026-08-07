#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_task_wdt.h"

#include "michi_state.h"

#define TAG "michi_state"

/* FSM task: above app_main (1), below the I2S consumer (8) so audio never
 * starves; the task only receives, logs and dispatches - it must be free to
 * drain the queue. */
#define MICHI_STATE_TASK_PRIORITY 5

/* One bit per target state: the transition table is a mask per source state
 * (bit set = transition allowed). Single source of truth for
 * michi_state_request() validation. */
#define ST_BIT(s) (1u << (s))

_Static_assert(MICHI_STATE_COUNT <= 32, "state bitmask overflow");

static const uint32_t s_transitions[MICHI_STATE_COUNT] = {
    [MICHI_STATE_BOOTING] = ST_BIT(MICHI_STATE_SELF_TEST) |
                            ST_BIT(MICHI_STATE_FATAL_ERROR),
    [MICHI_STATE_SELF_TEST] = ST_BIT(MICHI_STATE_IDLE) |
                              ST_BIT(MICHI_STATE_RECOVERABLE_ERROR) |
                              ST_BIT(MICHI_STATE_FATAL_ERROR),
    [MICHI_STATE_UNPROVISIONED] = ST_BIT(MICHI_STATE_PROVISIONING) |
                                  ST_BIT(MICHI_STATE_WIFI_CONNECTING) |
                                  ST_BIT(MICHI_STATE_PAIRING) |
                                  ST_BIT(MICHI_STATE_RECOVERABLE_ERROR) |
                                  ST_BIT(MICHI_STATE_FATAL_ERROR),
    [MICHI_STATE_PROVISIONING] = ST_BIT(MICHI_STATE_WIFI_CONNECTING) |
                                 ST_BIT(MICHI_STATE_UNPROVISIONED) |
                                 ST_BIT(MICHI_STATE_RECOVERABLE_ERROR) |
                                 ST_BIT(MICHI_STATE_FATAL_ERROR),
    [MICHI_STATE_WIFI_CONNECTING] = ST_BIT(MICHI_STATE_IDLE) |
                                    ST_BIT(MICHI_STATE_UNPROVISIONED) |
                                    ST_BIT(MICHI_STATE_RECOVERABLE_ERROR) |
                                    ST_BIT(MICHI_STATE_FATAL_ERROR),
    [MICHI_STATE_IDLE] = ST_BIT(MICHI_STATE_UNPROVISIONED) |
                         ST_BIT(MICHI_STATE_PROVISIONING) |
                         ST_BIT(MICHI_STATE_WIFI_CONNECTING) |
                         ST_BIT(MICHI_STATE_PAIRING) |
                         ST_BIT(MICHI_STATE_SESSION_PENDING) |
                         ST_BIT(MICHI_STATE_UPDATING) |
                         ST_BIT(MICHI_STATE_RECOVERABLE_ERROR) |
                         ST_BIT(MICHI_STATE_FATAL_ERROR),
    [MICHI_STATE_PAIRING] = ST_BIT(MICHI_STATE_IDLE) |
                            ST_BIT(MICHI_STATE_RECOVERABLE_ERROR) |
                            ST_BIT(MICHI_STATE_FATAL_ERROR),
    [MICHI_STATE_SESSION_PENDING] = ST_BIT(MICHI_STATE_BUFFERING) |
                                    ST_BIT(MICHI_STATE_IDLE) |
                                    ST_BIT(MICHI_STATE_RECOVERABLE_ERROR) |
                                    ST_BIT(MICHI_STATE_FATAL_ERROR),
    [MICHI_STATE_BUFFERING] = ST_BIT(MICHI_STATE_PLAYING) |
                              ST_BIT(MICHI_STATE_IDLE) |
                              ST_BIT(MICHI_STATE_RECOVERABLE_ERROR) |
                              ST_BIT(MICHI_STATE_FATAL_ERROR),
    [MICHI_STATE_PLAYING] = ST_BIT(MICHI_STATE_PAUSED) |
                            ST_BIT(MICHI_STATE_BUFFERING) |
                            ST_BIT(MICHI_STATE_IDLE) |
                            ST_BIT(MICHI_STATE_UPDATING) |
                            ST_BIT(MICHI_STATE_RECOVERABLE_ERROR) |
                            ST_BIT(MICHI_STATE_FATAL_ERROR),
    [MICHI_STATE_PAUSED] = ST_BIT(MICHI_STATE_PLAYING) |
                           ST_BIT(MICHI_STATE_IDLE) |
                           ST_BIT(MICHI_STATE_UPDATING) |
                           ST_BIT(MICHI_STATE_RECOVERABLE_ERROR) |
                           ST_BIT(MICHI_STATE_FATAL_ERROR),
    [MICHI_STATE_UPDATING] = ST_BIT(MICHI_STATE_IDLE) |
                             ST_BIT(MICHI_STATE_RECOVERABLE_ERROR) |
                             ST_BIT(MICHI_STATE_FATAL_ERROR),
    [MICHI_STATE_RECOVERABLE_ERROR] = ST_BIT(MICHI_STATE_IDLE) |
                                      ST_BIT(MICHI_STATE_UNPROVISIONED) |
                                      ST_BIT(MICHI_STATE_WIFI_CONNECTING) |
                                      ST_BIT(MICHI_STATE_FATAL_ERROR),
    [MICHI_STATE_FATAL_ERROR] = 0, /* terminal: no outgoing transitions */
};

/* Event -> transition mapping. `any_data` ignores the payload; otherwise
 * data must match exactly. `from` is the state the event must arrive in:
 * the lookup keys on it, so an event only matches the entry whose `from`
 * equals the state current at dispatch time - several entries may share an
 * id (PAIRING_STARTED from IDLE and from UNPROVISIONED); the from key picks
 * the reachable one. An event with no entry for the current state is
 * out-of-contract: no transition, the event itself is still broadcast to
 * observers and a warn is logged. SELF_TEST_DONE carries the self-test
 * overall result as data for observers, but ANY data drives
 * SELF_TEST->IDLE: the result is surfaced by app_main's log, and
 * RECOVERABLE_ERROR has no boot path - it is reserved for runtime producers
 * arriving from phase 9. */
typedef struct {
    michi_event_id_t id;
    uint32_t data;
    bool any_data;
    michi_state_t from;
    michi_state_t target;
} michi_event_map_t;

static const michi_event_map_t s_event_map[] = {
    { MICHI_EVENT_BOOT_COMPLETE,   0, true, MICHI_STATE_BOOTING,           MICHI_STATE_SELF_TEST },
    { MICHI_EVENT_SELF_TEST_DONE,  0, true, MICHI_STATE_SELF_TEST,         MICHI_STATE_IDLE },
    { MICHI_EVENT_RECOVER,         0, true, MICHI_STATE_RECOVERABLE_ERROR, MICHI_STATE_IDLE },
    /* Phase 8 (physical pairing button, michi_button): the same event
     * arrives from two source states; the from-keyed lookup keeps both
     * entries reachable. */
    { MICHI_EVENT_PAIRING_STARTED, 0, true, MICHI_STATE_IDLE,             MICHI_STATE_PAIRING },
    { MICHI_EVENT_PAIRING_STARTED, 0, true, MICHI_STATE_UNPROVISIONED,    MICHI_STATE_PAIRING },
    /* Phase 9 (network, michi_wifi): MICHI_EVENT_WIFI_CONNECTED is
     * broadcast-only (observers see the L2 link); the mapped events below
     * drive the unprovisioned -> connecting -> ready cycle. The reconnect
     * with backoff lives in michi_wifi (esp_timer one-shot, P0-11): the
     * FSM only sees the DISCONNECTED event, never the raw wifi events
     * from the esp_event handlers. */
    { MICHI_EVENT_WIFI_PROVISIONED,  0, true, MICHI_STATE_UNPROVISIONED,  MICHI_STATE_WIFI_CONNECTING },
    { MICHI_EVENT_WIFI_DISCONNECTED, 0, true, MICHI_STATE_IDLE,           MICHI_STATE_WIFI_CONNECTING },
    { MICHI_EVENT_NETWORK_READY,     0, true, MICHI_STATE_WIFI_CONNECTING, MICHI_STATE_IDLE },
    { MICHI_EVENT_WIFI_PROV_FAILED,  0, true, MICHI_STATE_WIFI_CONNECTING, MICHI_STATE_UNPROVISIONED },
    /* Phase 10 (pairing, michi_pairing): any window close (expiry,
     * paired, attempt exhaustion, explicit close) returns the FSM from
     * PAIRING to IDLE. */
    { MICHI_EVENT_PAIRING_WINDOW_CLOSED, 0, true, MICHI_STATE_PAIRING,     MICHI_STATE_IDLE },
    /* Phase 12 (sessions, michi_session): SESSION_STARTED is posted once
     * per lifecycle step (negotiated -> engine starting -> running); the
     * from-keyed lookup advances the chain one step per post, the same
     * pattern as PAIRING_STARTED. BUFFERING is modeled retrospectively
     * (the engine's real prefill is internal to michi_audio). CLOSED
     * ends the session from any session state; PAUSED/RESUMED switch
     * between PLAYING and PAUSED (pause stops the engine, state kept). */
    { MICHI_EVENT_SESSION_STARTED, 0, true, MICHI_STATE_IDLE,            MICHI_STATE_SESSION_PENDING },
    { MICHI_EVENT_SESSION_STARTED, 0, true, MICHI_STATE_SESSION_PENDING, MICHI_STATE_BUFFERING },
    { MICHI_EVENT_SESSION_STARTED, 0, true, MICHI_STATE_BUFFERING,       MICHI_STATE_PLAYING },
    { MICHI_EVENT_SESSION_CLOSED,  0, true, MICHI_STATE_SESSION_PENDING, MICHI_STATE_IDLE },
    { MICHI_EVENT_SESSION_CLOSED,  0, true, MICHI_STATE_BUFFERING,       MICHI_STATE_IDLE },
    { MICHI_EVENT_SESSION_CLOSED,  0, true, MICHI_STATE_PLAYING,         MICHI_STATE_IDLE },
    { MICHI_EVENT_SESSION_CLOSED,  0, true, MICHI_STATE_PAUSED,          MICHI_STATE_IDLE },
    { MICHI_EVENT_SESSION_PAUSED,  0, true, MICHI_STATE_PLAYING,         MICHI_STATE_PAUSED },
    { MICHI_EVENT_SESSION_RESUMED, 0, true, MICHI_STATE_PAUSED,          MICHI_STATE_PLAYING },
};

/* Structural coverage: every s_event_map entry must have an
 * EVENT_MAP_TRANSITION_CHECK below. sizeof on a static array IS an integer
 * constant expression, so a new entry without its check fails this assert
 * at compile time. */
#define MICHI_EVENT_MAP_CHECK_COUNT 19
_Static_assert(sizeof(s_event_map) / sizeof(s_event_map[0]) ==
                   MICHI_EVENT_MAP_CHECK_COUNT,
               "every event map entry needs a transition check");

/* Phase 5 follow-up: every event-map (from, target) pair must be a valid
 * transition - the mapping must never bypass s_transitions.
 *
 * GCC 13 (this toolchain) rejects reads of static const arrays inside
 * _Static_assert (they are not integer constant expressions), so the check
 * is expressed as a designated initializer index: a pair missing from the
 * transition table indexes [1] into a char[1] and fails to compile with
 * "array index in initializer exceeds array bounds". One array per entry;
 * a new s_event_map entry must add one. */
#define EVENT_MAP_TRANSITION_CHECK(entry, name) \
    static char name[1] __attribute__((unused)) = { \
        [(s_transitions[s_event_map[entry].from] & ST_BIT(s_event_map[entry].target)) ? 0 : 1] = 0 \
    }
EVENT_MAP_TRANSITION_CHECK(0, s_evmap_check_0);
EVENT_MAP_TRANSITION_CHECK(1, s_evmap_check_1);
EVENT_MAP_TRANSITION_CHECK(2, s_evmap_check_2);
EVENT_MAP_TRANSITION_CHECK(3, s_evmap_check_3);
EVENT_MAP_TRANSITION_CHECK(4, s_evmap_check_4);
EVENT_MAP_TRANSITION_CHECK(5, s_evmap_check_5);
EVENT_MAP_TRANSITION_CHECK(6, s_evmap_check_6);
EVENT_MAP_TRANSITION_CHECK(7, s_evmap_check_7);
EVENT_MAP_TRANSITION_CHECK(8, s_evmap_check_8);
EVENT_MAP_TRANSITION_CHECK(9, s_evmap_check_9);
EVENT_MAP_TRANSITION_CHECK(10, s_evmap_check_10);
EVENT_MAP_TRANSITION_CHECK(11, s_evmap_check_11);
EVENT_MAP_TRANSITION_CHECK(12, s_evmap_check_12);
EVENT_MAP_TRANSITION_CHECK(13, s_evmap_check_13);
EVENT_MAP_TRANSITION_CHECK(14, s_evmap_check_14);
EVENT_MAP_TRANSITION_CHECK(15, s_evmap_check_15);
EVENT_MAP_TRANSITION_CHECK(16, s_evmap_check_16);
EVENT_MAP_TRANSITION_CHECK(17, s_evmap_check_17);
EVENT_MAP_TRANSITION_CHECK(18, s_evmap_check_18);
#undef EVENT_MAP_TRANSITION_CHECK

typedef struct {
    michi_event_id_t filter; /* 0 = all events */
    michi_state_observer_fn fn;
} michi_observer_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static volatile michi_state_t s_current;
static volatile bool s_initialized;
static volatile uint32_t s_drops;

static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_obs_mux = portMUX_INITIALIZER_UNLOCKED;
static michi_observer_t s_observers[CONFIG_MICHI_STATE_MAX_OBSERVERS];

static const char *const s_state_names[MICHI_STATE_COUNT] = {
    [MICHI_STATE_BOOTING] = "BOOTING",
    [MICHI_STATE_SELF_TEST] = "SELF_TEST",
    [MICHI_STATE_UNPROVISIONED] = "UNPROVISIONED",
    [MICHI_STATE_PROVISIONING] = "PROVISIONING",
    [MICHI_STATE_WIFI_CONNECTING] = "WIFI_CONNECTING",
    [MICHI_STATE_IDLE] = "IDLE",
    [MICHI_STATE_PAIRING] = "PAIRING",
    [MICHI_STATE_SESSION_PENDING] = "SESSION_PENDING",
    [MICHI_STATE_BUFFERING] = "BUFFERING",
    [MICHI_STATE_PLAYING] = "PLAYING",
    [MICHI_STATE_PAUSED] = "PAUSED",
    [MICHI_STATE_UPDATING] = "UPDATING",
    [MICHI_STATE_RECOVERABLE_ERROR] = "RECOVERABLE_ERROR",
    [MICHI_STATE_FATAL_ERROR] = "FATAL_ERROR",
};

static const char *state_name(michi_state_t s)
{
    if (s >= MICHI_STATE_COUNT) {
        return "UNKNOWN";
    }
    return s_state_names[s];
}

static bool transition_valid(michi_state_t from, michi_state_t to)
{
    if (from >= MICHI_STATE_COUNT || to >= MICHI_STATE_COUNT) {
        return false;
    }
    return (s_transitions[from] & ST_BIT(to)) != 0;
}

static void dispatch_observers(const michi_event_t *ev)
{
    michi_state_observer_fn snapshot[CONFIG_MICHI_STATE_MAX_OBSERVERS];
    size_t n = 0;

    /* Snapshot under the lock, invoke outside it: observers may post events
     * (allowed) and the FSM must never call observers holding a lock. */
    portENTER_CRITICAL(&s_obs_mux);
    for (size_t i = 0; i < CONFIG_MICHI_STATE_MAX_OBSERVERS; i++) {
        const michi_observer_t *o = &s_observers[i];
        if (o->fn != NULL && (o->filter == 0 || o->filter == ev->id)) {
            snapshot[n++] = o->fn;
        }
    }
    portEXIT_CRITICAL(&s_obs_mux);

    for (size_t i = 0; i < n; i++) {
        snapshot[i](ev);
    }
}

static void apply_transition(michi_state_t target, michi_state_t from)
{
    portENTER_CRITICAL(&s_state_mux);
    s_current = target;
    portEXIT_CRITICAL(&s_state_mux);

    ESP_LOGI(TAG, "state: from=%s to=%s", state_name(from), state_name(target));

    const michi_event_t ev = {
        .id = MICHI_EVENT_STATE_CHANGED,
        .data = (uint32_t)target,
        .from = (uint32_t)from,
    };
    dispatch_observers(&ev);
}

/* Lookup keyed on (id, data, from): an event only matches the entry whose
 * `from` is the state current at dispatch time. Several entries may share
 * an id (PAIRING_STARTED from IDLE and from UNPROVISIONED); the from key
 * picks the reachable one - a first-match lookup would make the second
 * entry unreachable. */
static const michi_event_map_t *find_event_map(michi_event_id_t id,
                                               uint32_t data,
                                               michi_state_t from)
{
    for (size_t i = 0; i < sizeof(s_event_map) / sizeof(s_event_map[0]); i++) {
        const michi_event_map_t *m = &s_event_map[i];
        if (m->id == id && m->from == from &&
            (m->any_data || m->data == data)) {
            return m;
        }
    }
    return NULL;
}

static bool map_exists_for_id(michi_event_id_t id)
{
    for (size_t i = 0; i < sizeof(s_event_map) / sizeof(s_event_map[0]); i++) {
        if (s_event_map[i].id == id) {
            return true;
        }
    }
    return false;
}

/* Transition request validated once at request() time; re-validated here
 * because the state may have changed while the event was queued. */
static void handle_transition_request(michi_state_t target)
{
    if (target >= MICHI_STATE_COUNT) {
        ESP_LOGW(TAG, "state: request rejected=target=%u out of range", (unsigned)target);
        return;
    }
    if (!transition_valid(s_current, target)) {
        ESP_LOGW(TAG, "state: request rejected=to=%s from=%s (state changed since request)",
                 state_name(target), state_name(s_current));
        return;
    }
    apply_transition(target, s_current);
}

static void state_task(void *arg)
{
    michi_event_t ev;

    for (;;) {
        /* Bounded receive so the task can feed the task watchdog while idle:
         * an observer that blocks past the WDT timeout triggers the watchdog
         * instead of a silent stall (5 s default). */
        if (xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(1000)) != pdTRUE) {
            esp_task_wdt_reset();
            continue;
        }
        esp_task_wdt_reset();

        /* Drops are counted by the producers (queue full); report and reset
         * once per dispatch so the log volume stays low. */
        portENTER_CRITICAL(&s_state_mux);
        const uint32_t drops = s_drops;
        s_drops = 0;
        portEXIT_CRITICAL(&s_state_mux);
        if (drops > 0) {
            ESP_LOGW(TAG, "state: events_dropped=%u", (unsigned)drops);
        }

        ESP_LOGI(TAG, "state: event=%d data=%u", (int)ev.id, (unsigned)ev.data);

        if (ev.id == MICHI_EVENT_TRANSITION_REQUEST) {
            /* Internal: never broadcast; the resulting STATE_CHANGED is. */
            handle_transition_request((michi_state_t)ev.data);
            continue;
        }

        /* Stamp dispatch-time context; for STATE_CHANGED the FSM rewrites it
         * with the authoritative previous state (apply_transition). */
        ev.from = (uint32_t)s_current;
        dispatch_observers(&ev);

        const michi_event_map_t *m = find_event_map(ev.id, ev.data,
                                                     s_current);
        if (m == NULL) {
            if (map_exists_for_id(ev.id)) {
                ESP_LOGW(TAG, "state: event=%d data=%u has no mapping from %s, broadcast only",
                         (int)ev.id, (unsigned)ev.data, state_name(s_current));
            }
            continue;
        }
        /* The from-keyed lookup guarantees m->from == s_current; the
         * remaining authority check is the transition table: the mapping
         * must not bypass s_transitions, so the (from, target) pairs of
         * s_event_map stay validated at dispatch time. */
        if (!transition_valid(s_current, m->target)) {
            ESP_LOGW(TAG, "state: event=%d data=%u dropped: transition to=%s from=%s not in table",
                     (int)ev.id, (unsigned)ev.data, state_name(m->target),
                     state_name(s_current));
            continue;
        }
        apply_transition(m->target, s_current);
    }
}

const char *michi_state_name(michi_state_t s)
{
    return state_name(s);
}

michi_state_t michi_state_get(void)
{
    michi_state_t s;

    portENTER_CRITICAL(&s_state_mux);
    s = s_current;
    portEXIT_CRITICAL(&s_state_mux);
    return s;
}

esp_err_t michi_state_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_queue = xQueueCreate(CONFIG_MICHI_STATE_QUEUE_LEN, sizeof(michi_event_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "init: queue allocation failed");
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_state_mux);
    s_current = MICHI_STATE_BOOTING;
    portEXIT_CRITICAL(&s_state_mux);

    BaseType_t rc = xTaskCreate(state_task, "michi_state",
                                CONFIG_MICHI_STATE_TASK_STACK_BYTES, NULL,
                                MICHI_STATE_TASK_PRIORITY, &s_task);
    if (rc != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        ESP_LOGE(TAG, "init: task creation failed");
        return ESP_ERR_NO_MEM;
    }

    /* Task watchdog: an observer that blocks past the WDT timeout triggers
     * the watchdog (5 s default) instead of a silent stall; the FSM task
     * feeds it from its bounded receive loop. */
    esp_err_t wdt_rc = esp_task_wdt_add(s_task);
    if (wdt_rc != ESP_OK) {
        ESP_LOGW(TAG, "init: task watchdog add failed (%s), FSM runs without watchdog",
                 esp_err_to_name(wdt_rc));
    }

    s_initialized = true;
    ESP_LOGI(TAG, "subsystem=state state=ok phase=5");
    return ESP_OK;
}

esp_err_t michi_state_post(michi_event_id_t id, uint32_t data)
{
    /* Internal reserved range: the transition-request event, the FSM-only
     * STATE_CHANGED broadcast and any id past the enum are not postable. */
    if (id == MICHI_EVENT_STATE_CHANGED ||
        id >= MICHI_EVENT_TRANSITION_REQUEST) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        ESP_LOGW(TAG, "post: state machine not initialized, event=%d dropped",
                 (int)id);
        return ESP_ERR_INVALID_STATE;
    }
    const michi_event_t ev = { .id = id, .data = data, .from = 0 };
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_state_mux);
        s_drops++;
        portEXIT_CRITICAL(&s_state_mux);
        ESP_LOGW(TAG, "post: queue full, event=%d dropped", (int)id);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t michi_state_post_from_isr(michi_event_id_t id, uint32_t data)
{
    if (id == MICHI_EVENT_STATE_CHANGED ||
        id >= MICHI_EVENT_TRANSITION_REQUEST) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    const michi_event_t ev = { .id = id, .data = data, .from = 0 };
    BaseType_t hpw = pdFALSE;
    if (xQueueSendFromISR(s_queue, &ev, &hpw) != pdTRUE) {
        /* ISR context cannot log: counted, the FSM task reports periodically. */
        portENTER_CRITICAL_ISR(&s_state_mux);
        s_drops++;
        portEXIT_CRITICAL_ISR(&s_state_mux);
        return ESP_ERR_TIMEOUT;
    }
    if (hpw) {
        portYIELD_FROM_ISR();
    }
    return ESP_OK;
}

esp_err_t michi_state_request(michi_state_t target)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "state: request rejected=state machine not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (target >= MICHI_STATE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    michi_state_t cur = michi_state_get();
    if (!transition_valid(cur, target)) {
        ESP_LOGW(TAG, "state: request rejected=to=%s from=%s",
                 state_name(target), state_name(cur));
        return ESP_ERR_INVALID_STATE;
    }

    const michi_event_t ev = {
        .id = MICHI_EVENT_TRANSITION_REQUEST,
        .data = (uint32_t)target,
        .from = 0,
    };
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "state: request rejected=queue full to=%s",
                 state_name(target));
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t michi_state_register_observer(michi_event_id_t filter,
                                        michi_state_observer_fn fn)
{
    if (fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_obs_mux);
    for (size_t i = 0; i < CONFIG_MICHI_STATE_MAX_OBSERVERS; i++) {
        michi_observer_t *o = &s_observers[i];
        if (o->fn == fn && o->filter == filter) {
            portEXIT_CRITICAL(&s_obs_mux);
            return ESP_OK; /* idempotent */
        }
    }
    for (size_t i = 0; i < CONFIG_MICHI_STATE_MAX_OBSERVERS; i++) {
        michi_observer_t *o = &s_observers[i];
        if (o->fn == NULL) {
            o->fn = fn;
            o->filter = filter;
            portEXIT_CRITICAL(&s_obs_mux);
            return ESP_OK;
        }
    }
    portEXIT_CRITICAL(&s_obs_mux);
    ESP_LOGW(TAG, "register: observer table full (max %d)",
             (int)CONFIG_MICHI_STATE_MAX_OBSERVERS);
    return ESP_ERR_NO_MEM;
}
