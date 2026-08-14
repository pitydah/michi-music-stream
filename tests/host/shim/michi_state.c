/* Fake michi_state bus for host-side tests (see michi_state.h).
 * TEST-ONLY. */

#include "michi_state.h"

#include <string.h>

#define TEST_STATE_EVENT_MAX 64

static michi_event_id_t s_events[TEST_STATE_EVENT_MAX];
static size_t s_count;

void test_state_reset(void)
{
    memset(s_events, 0, sizeof(s_events));
    s_count = 0;
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

esp_err_t michi_state_post(michi_event_id_t id, uint32_t data)
{
    (void)data;
    if (s_count >= TEST_STATE_EVENT_MAX) {
        return ESP_ERR_TIMEOUT;
    }
    s_events[s_count++] = id;
    return ESP_OK;
}
