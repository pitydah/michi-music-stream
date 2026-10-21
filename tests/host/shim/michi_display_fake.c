/* Fake michi_display for host-side tests: implements the ONLY function
 * the session layer calls (michi_display_clear_now_playing) from the
 * REAL michi_display.h. TEST-ONLY: never compiled into firmware. */

#include "michi_display.h"
#include <string.h>

static int s_clear_count;
static int s_update_count;
static char s_last_title[80];

void test_display_reset(void)
{
    s_clear_count = 0;
    s_update_count = 0;
    s_last_title[0] = '\0';
}

int test_display_clear_count(void)
{
    return s_clear_count;
}

int test_display_update_count(void) { return s_update_count; }
const char *test_display_last_title(void) { return s_last_title; }

esp_err_t michi_display_update_now_playing(const char *source,
                                           const char *title,
                                           const char *artist)
{
    (void)source;
    (void)artist;
    if (title == NULL) return ESP_ERR_INVALID_ARG;
    s_update_count++;
    strncpy(s_last_title, title, sizeof(s_last_title) - 1);
    s_last_title[sizeof(s_last_title) - 1] = '\0';
    return ESP_OK;
}

esp_err_t michi_display_clear_now_playing(void)
{
    s_clear_count++;
    return ESP_OK;
}

esp_err_t michi_display_set_pairing_overlay(int overlay)
{
    (void)overlay;
    return ESP_OK; /* ESP_OK */
}
