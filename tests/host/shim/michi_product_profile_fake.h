#pragma once
/* Test double for michi_product_profile (host-side tests of the
 * michi_discovery runtime). The REAL michi_product_profile.c depends on
 * michi_dac/michi_board/esp_partition (not host-compiled); this fake
 * mirrors michi_product_profile_get()/tier_name() - the two entry
 * points michi_discovery.c uses. capabilities.c (the canonical
 * capability table) is still compiled REAL. TEST-ONLY. */

#include <stdbool.h>

#include "michi_product_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reset to the default profile: name "Michi Test", tier STANDARD. */
void test_profile_reset(void);

/* Configure the visible device name (<= 31 chars). */
void test_profile_set_name(const char *name);

/* Configure the product tier. */
void test_profile_set_tier(michi_product_tier_t tier);

#ifdef __cplusplus
}
#endif
