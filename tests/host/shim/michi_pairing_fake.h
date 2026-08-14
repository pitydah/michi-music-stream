#pragma once
/* Test double for the pairing side of the button contract: call counters
 * for the two michi_pairing entry points the button uses
 * (michi_pairing_open_window / michi_pairing_erase_all). The REAL
 * pairing component is already host-tested end to end by
 * test_michi_pairing (including erase_all); the button test only needs
 * to prove the WIRING - that a factory reset actually calls the pairing
 * wipe - so the real registry machinery is not duplicated here.
 * TEST-ONLY: never compiled into firmware. */

#ifdef __cplusplus
extern "C" {
#endif

/* --- test hooks --- */

void test_pairing_fake_reset(void);
int test_pairing_open_window_calls(void);
int test_pairing_erase_all_calls(void);

#ifdef __cplusplus
}
#endif
