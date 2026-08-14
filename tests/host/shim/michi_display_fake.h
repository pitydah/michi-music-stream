#pragma once
/* Test hooks for shim/michi_display_fake.c. TEST-ONLY. */

#ifdef __cplusplus
extern "C" {
#endif

void test_display_reset(void);
int test_display_clear_count(void);

#ifdef __cplusplus
}
#endif
