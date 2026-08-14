/* Fake michi_state bus for host-side tests (see michi_state.h).
 * TEST-ONLY. */

#include "michi_state.h"

#include <string.h>

#define TEST_STATE_EVENT_MAX 64

static michi_event_id_t s_events[TEST_STATE_EVENT_MAX];
static size_t s_count;
static michi_state_t s_state = MICHI_STATE_IDLE;

void test_state_reset(void)
{
    memset(s_events, 0, sizeof(s_events));
    s_count = 0;
    s_state = MICHI_STATE_IDLE;
}

size_t test_state_post_count(michi_event_id_t id)
{
    size_t n = 0;
    for (size_t i = 0; i < s_count; i++) {
        if (s_events[i] == id) {
            n++;
        }
    }
    return n;
}

bool test_state_saw_event(michi_event_id_t id)
{
    return test_state_post_count(id) > 0;
}

void test_state_set(michi_state_t st)
{
    s_state = st;
}

esp_err_t michi_state_post(michi_event_id_t id, uint32_t data)
{
    (void)data;
    if (s_count >= TEST_STATE_EVENT_MAX) {
        return ESP_ERR_TIMEOUT;
    }
    s_events[s_count++] = id;
    /* Model the real FSM from-keyed transitions so the session layer's
     * reconciliation sees a faithful bus: SESSION_STARTED drives
     * IDLE -> SESSION_PENDING -> BUFFERING -> PLAYING; CLOSED returns to
     * IDLE; PAUSED/RESUMED flip PLAYING <-> PAUSED. */
    switch (id) {
    case MICHI_EVENT_SESSION_STARTED:
        if (s_state == MICHI_STATE_IDLE) {
            s_state = MICHI_STATE_SESSION_PENDING;
        } else if (s_state == MICHI_STATE_SESSION_PENDING) {
            s_state = MICHI_STATE_BUFFERING;
        } else if (s_state == MICHI_STATE_BUFFERING) {
            s_state = MICHI_STATE_PLAYING;
        }
        break;
    case MICHI_EVENT_SESSION_CLOSED:
        s_state = MICHI_STATE_IDLE;
        break;
    case MICHI_EVENT_SESSION_PAUSED:
        if (s_state == MICHI_STATE_PLAYING) {
            s_state = MICHI_STATE_PAUSED;
        }
        break;
    case MICHI_EVENT_SESSION_RESUMED:
        if (s_state == MICHI_STATE_PAUSED) {
            s_state = MICHI_STATE_PLAYING;
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

esp_err_t michi_state_report_error(michi_event_id_t event, uint32_t data)
{
    (void)data;
    return michi_state_post(event, data);
}

michi_state_t michi_state_get(void)
{
    return s_state;
}

const char *michi_state_name(michi_state_t s)
{
    switch (s) {
    case MICHI_STATE_BOOTING:            return "BOOTING";
    case MICHI_STATE_SELF_TEST:          return "SELF_TEST";
    case MICHI_STATE_UNPROVISIONED:      return "UNPROVISIONED";
    case MICHI_STATE_PROVISIONING:       return "PROVISIONING";
    case MICHI_STATE_WIFI_CONNECTING:    return "WIFI_CONNECTING";
    case MICHI_STATE_IDLE:               return "IDLE";
    case MICHI_STATE_PAIRING:            return "PAIRING";
    case MICHI_STATE_SESSION_PENDING:    return "SESSION_PENDING";
    case MICHI_STATE_BUFFERING:          return "BUFFERING";
    case MICHI_STATE_PLAYING:            return "PLAYING";
    case MICHI_STATE_PAUSED:             return "PAUSED";
    case MICHI_STATE_UPDATING:           return "UPDATING";
    case MICHI_STATE_RECOVERABLE_ERROR:  return "RECOVERABLE_ERROR";
    case MICHI_STATE_FATAL_ERROR:        return "FATAL_ERROR";
    case MICHI_STATE_COUNT:              return "COUNT";
    }
    return "UNKNOWN";
}
