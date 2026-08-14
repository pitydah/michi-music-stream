#pragma once
/* Shim for host-side tests: esp_system stand-in. esp_restart() records
 * the call instead of rebooting the host. TEST-ONLY: never compiled into
 * firmware. */

#ifdef __cplusplus
extern "C" {
#endif

/* --- test hooks --- */

int test_esp_restart_count(void);
void test_esp_restart_count_reset(void);

/* --- fake esp_system API (used by the firmware sources under test) --- */

void esp_restart(void);

#ifdef __cplusplus
}
#endif
