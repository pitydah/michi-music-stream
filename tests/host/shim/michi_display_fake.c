/* Fake michi_display for host-side tests: implements the ONLY function
 * the session layer calls (michi_display_clear_now_playing) from the
 * REAL michi_display.h. TEST-ONLY: never compiled into firmware. */

#include "michi_display.h"

static int s_clear_count;

void test_display_reset(void)
{
    s_clear_count = 0;
}

int test_display_clear_count(void)
{
    return s_clear_count;
}

esp_err_t michi_display_clear_now_playing(void)
{
    s_clear_count++;
    return ESP_OK;
}
