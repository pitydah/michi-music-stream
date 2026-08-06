#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

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

static const uint32_t s_transitions[MICHI_STATE_COUNT] = {
    [MICHI_STATE_BOOTING] = ST_BIT(MICHI_STATE_SELF_TEST),
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
 * data must match exactly (SELF_TEST_DONE: 1=ok -> IDLE, 0=fail ->
 * RECOVERABLE_ERROR). `from` is the state the event must arrive in: a
 * mismatched arrival is out-of-contract, logged and dropped (no transition;
 * the event itself is still broadcast). */
typedef struct {
    michi_event_id_t id;
    uint32_t data;
    bool any_data;
    michi_state_t from;
    michi_state_t target;
} michi_event_map_t;

static const michi_event_map_t s_event_map[] = {
    { MICHI_EVENT_BOOT_COMPLETE,  0,  true, MICHI_STATE_BOOTING,           MICHI_STATE_SELF_TEST },
    { MICHI_EVENT_SELF_TEST_DONE, 1,  false, MICHI_STATE_SELF_TEST,        MICHI_STATE_IDLE },
    { MICHI_EVENT_SELF_TEST_DONE, 0,  false, MICHI_STATE_SELF_TEST,        MICHI_STATE_RECOVERABLE_ERROR },
    { MICHI_EVENT_RECOVER,        0,  true,  MICHI_STATE_RECOVERABLE_ERROR, MICHI_STATE_IDLE },
};

typedef struct {
    michi_event_id_t filter; /* 0 = all events */
    michi_state_observer_fn fn;
} michi_observer_t;

static QueueHandle_t s_queue;
static volatile michi_state_t s_current;
static volatile bool s_initialized;

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

static const michi_event_map_t *find_event_map(michi_event_id_t id, uint32_t data)
{
    for (size_t i = 0; i < sizeof(s_event_map) / sizeof(s_event_map[0]); i++) {
        const michi_event_map_t *m = &s_event_map[i];
        if (m->id == id && (m->any_data || m->data == data)) {
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
        ESP_LOGW(TAG, "state: request target=%u out of range", (unsigned)target);
        return;
    }
    if (!transition_valid(s_current, target)) {
        ESP_LOGW(TAG, "state: request to=%s invalid from=%s (state changed since request)",
                 state_name(target), state_name(s_current));
        return;
    }
    apply_transition(target, s_current);
}

static void state_task(void *arg)
{
    michi_event_t ev;

    for (;;) {
        if (xQueueReceive(s_queue, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "state: current=%s event=%d data=%u",
                 state_name(s_current), (int)ev.id, (unsigned)ev.data);

        if (ev.id == MICHI_EVENT__TRANSITION_REQUEST) {
            /* Internal: never broadcast; the resulting STATE_CHANGED is. */
            handle_transition_request((michi_state_t)ev.data);
            continue;
        }

        /* Stamp dispatch-time context; for STATE_CHANGED the FSM rewrites it
         * with the authoritative previous state (apply_transition). */
        ev.from = (uint32_t)s_current;
        dispatch_observers(&ev);

        const michi_event_map_t *m = find_event_map(ev.id, ev.data);
        if (m == NULL) {
            if (map_exists_for_id(ev.id)) {
                ESP_LOGW(TAG, "state: event=%d data=%u has no mapping, broadcast only",
                         (int)ev.id, (unsigned)ev.data);
            }
            continue;
        }
        if (m->from != s_current) {
            ESP_LOGW(TAG, "state: event=%d dropped: expected state=%s but current=%s",
                     (int)ev.id, state_name(m->from), state_name(s_current));
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
                                MICHI_STATE_TASK_PRIORITY, NULL);
    if (rc != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        ESP_LOGE(TAG, "init: task creation failed");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "subsystem=state state=ok phase=5");
    return ESP_OK;
}

esp_err_t michi_state_post(michi_event_id_t id, uint32_t data)
{
    if (id == MICHI_EVENT__TRANSITION_REQUEST) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        ESP_LOGW(TAG, "post: state machine not initialized, event=%d dropped",
                 (int)id);
        return ESP_ERR_INVALID_STATE;
    }
    const michi_event_t ev = { .id = id, .data = data, .from = 0 };
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "post: queue full, event=%d dropped", (int)id);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t michi_state_post_from_isr(michi_event_id_t id, uint32_t data)
{
    if (id == MICHI_EVENT__TRANSITION_REQUEST) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    const michi_event_t ev = { .id = id, .data = data, .from = 0 };
    BaseType_t hpw = pdFALSE;
    if (xQueueSendFromISR(s_queue, &ev, &hpw) != pdTRUE) {
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
        ESP_LOGW(TAG, "request: state machine not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (target >= MICHI_STATE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    michi_state_t cur = michi_state_get();
    if (!transition_valid(cur, target)) {
        ESP_LOGW(TAG, "state: request to=%s invalid from=%s",
                 state_name(target), state_name(cur));
        return ESP_ERR_INVALID_STATE;
    }

    const michi_event_t ev = {
        .id = MICHI_EVENT__TRANSITION_REQUEST,
        .data = (uint32_t)target,
        .from = 0,
    };
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "state: request queue full, to=%s dropped",
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
