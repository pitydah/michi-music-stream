/* Fake esp_restart for host-side tests (see esp_system.h). TEST-ONLY. */

#include "esp_system.h"

static int s_restart_count;

int test_esp_restart_count(void)
{
    return s_restart_count;
}

void test_esp_restart_count_reset(void)
{
    s_restart_count = 0;
}

void esp_restart(void)
{
    s_restart_count++;
}
