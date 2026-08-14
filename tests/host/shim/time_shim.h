#pragma once
/* Shim for host-side tests: fake wall clock backing the time()
 * override (time_shim.c). The clock is set by the esp_netif_sntp shim
 * on every test_sntp_fire_sync (like SNTP setting the system time) and
 * can be advanced manually to simulate the RTC ticking during an
 * outage. TEST-ONLY: never compiled into firmware. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Current fake wall time (Unix seconds); 0 before the first sync. */
uint64_t test_time_get_sec(void);

/* Set the fake wall clock (called by the SNTP shim on a sync). */
void test_time_set_sec(uint64_t unix_sec);

/* Advance the fake wall clock (RTC ticking during an outage). */
void test_time_advance_sec(uint64_t sec);

/* Reset to 0. */
void test_time_reset(void);

#ifdef __cplusplus
}
#endif
