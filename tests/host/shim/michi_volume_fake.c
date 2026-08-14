/* Fake michi_volume for host-side tests: implements the REAL
 * michi_volume.h against a plain variable (the digital path; clamping
 * mirrors the firmware API). TEST-ONLY: never compiled into firmware. */

#include "michi_volume.h"

static uint8_t s_volume;

void test_volume_reset(void)
{
    s_volume = 0;
}

uint8_t test_volume_value(void)
{
    return s_volume;
}

esp_err_t michi_volume_set(uint8_t v)
{
    s_volume = v > 100 ? 100 : v;
    return ESP_OK;
}

uint8_t michi_volume_get(void)
{
    return s_volume;
}
