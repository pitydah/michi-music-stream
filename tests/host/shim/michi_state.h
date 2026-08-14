#pragma once
/* Shim for host-side tests: michi_state stand-in. The pairing component
 * posts MICHI_EVENT_PAIRING_WINDOW_CLOSED on window close; the session
 * component posts the SESSION_* chain and queries michi_state_get() for
 * the OTA gate and the FSM reconciliation. The fake bus records every
 * post (tests assert the events). TEST-ONLY: never compiled into
 * firmware. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MICHI_EVENT_BOOT_COMPLETE = 1,
    MICHI_EVENT_SELF_TEST_DONE,
    MICHI_EVENT_STATE_CHANGED,
    MICHI_EVENT_ERROR,
    MICHI_EVENT_RECOVER,
    MICHI_EVENT_WIFI_CONNECTED,
    MICHI_EVENT_WIFI_DISCONNECTED,
    MICHI_EVENT_WIFI_PROVISIONED,
    MICHI_EVENT_WIFI_PROV_FAILED,
    MICHI_EVENT_NETWORK_READY,
    MICHI_EVENT_PAIRING_STARTED,
    MICHI_EVENT_PAIRING_WINDOW_CLOSED,
    MICHI_EVENT_SESSION_STARTED,
    MICHI_EVENT_SESSION_CLOSED,
    MICHI_EVENT_SESSION_PAUSED,
    MICHI_EVENT_SESSION_RESUMED,
} michi_event_id_t;

typedef enum {
    MICHI_STATE_BOOTING = 0,
    MICHI_STATE_SELF_TEST,
    MICHI_STATE_UNPROVISIONED,
    MICHI_STATE_PROVISIONING,
    MICHI_STATE_WIFI_CONNECTING,
    MICHI_STATE_IDLE,
    MICHI_STATE_PAIRING,
    MICHI_STATE_SESSION_PENDING,
    MICHI_STATE_BUFFERING,
    MICHI_STATE_PLAYING,
    MICHI_STATE_PAUSED,
    MICHI_STATE_UPDATING,
    MICHI_STATE_RECOVERABLE_ERROR,
    MICHI_STATE_FATAL_ERROR,
    MICHI_STATE_COUNT,
} michi_state_t;

/* --- test hooks --- */

void test_state_reset(void);
size_t test_state_post_count(michi_event_id_t id);
bool test_state_saw_event(michi_event_id_t id);
void test_state_set(michi_state_t st);

/* --- fake bus (used by the firmware sources under test) --- */

esp_err_t michi_state_post(michi_event_id_t id, uint32_t data);
esp_err_t michi_state_report_error(michi_event_id_t event, uint32_t data);
michi_state_t michi_state_get(void);
const char *michi_state_name(michi_state_t s);

#ifdef __cplusplus
}
#endif
