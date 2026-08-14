#pragma once
/* Test hooks for shim/michi_volume_fake.c. TEST-ONLY. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void test_volume_reset(void);
uint8_t test_volume_value(void);

#ifdef __cplusplus
}
#endif
