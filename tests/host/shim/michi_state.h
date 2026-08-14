#pragma once
/* Shim for host-side tests: michi_state stand-in. The pairing component
 * posts MICHI_EVENT_PAIRING_WINDOW_CLOSED on window close; the fake bus
 * records every post (tests assert the close event). TEST-ONLY: never
 * compiled into firmware. */

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
} michi_event_id_t;

/* --- test hooks --- */

void test_state_reset(void);
size_t test_state_post_count(michi_event_id_t id);
bool test_state_saw_event(michi_event_id_t id);

/* --- fake bus (used by the firmware sources under test) --- */

esp_err_t michi_state_post(michi_event_id_t id, uint32_t data);

#ifdef __cplusplus
}
#endif
