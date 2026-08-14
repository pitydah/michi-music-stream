/* Call-counter test double for the pairing entry points the button uses
 * (see michi_pairing_fake.h). The REAL public header (michi_pairing.h)
 * is compiled in, so the signatures cannot drift. TEST-ONLY. */

#include "michi_pairing_fake.h"

#include "michi_pairing.h"

static int s_open_window_calls;
static int s_erase_all_calls;

void test_pairing_fake_reset(void)
{
    s_open_window_calls = 0;
    s_erase_all_calls = 0;
}

int test_pairing_open_window_calls(void)
{
    return s_open_window_calls;
}

int test_pairing_erase_all_calls(void)
{
    return s_erase_all_calls;
}

esp_err_t michi_pairing_open_window(void)
{
    s_open_window_calls++;
    return ESP_OK;
}

esp_err_t michi_pairing_erase_all(void)
{
    s_erase_all_calls++;
    return ESP_OK;
}
