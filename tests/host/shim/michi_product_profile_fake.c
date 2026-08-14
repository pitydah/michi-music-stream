/* Test double for michi_product_profile (see the header). TEST-ONLY:
 * never compiled into firmware. */

#include "michi_product_profile_fake.h"

#include <stdio.h>
#include <string.h>

static michi_product_profile_t s_profile;

static void profile_defaults(void)
{
    memset(&s_profile, 0, sizeof(s_profile));
    snprintf(s_profile.product_name, sizeof(s_profile.product_name),
             "%s", "Michi Test");
    s_profile.tier = MICHI_PRODUCT_STANDARD;
    s_profile.validated_sample_rate = 48000;
    s_profile.validated_bit_depth = 16;
}

void test_profile_reset(void)
{
    profile_defaults();
}

void test_profile_set_name(const char *name)
{
    snprintf(s_profile.product_name, sizeof(s_profile.product_name), "%s",
             name != NULL ? name : "");
}

void test_profile_set_tier(michi_product_tier_t tier)
{
    s_profile.tier = tier;
}

const michi_product_profile_t *michi_product_profile_get(void)
{
    static bool first = true;
    if (first) {
        first = false;
        profile_defaults();
    }
    return &s_profile;
}

const char *michi_product_profile_tier_name(void)
{
    switch (s_profile.tier) {
    case MICHI_PRODUCT_HIFI:
        return "hifi";
    case MICHI_PRODUCT_STANDARD:
        return "standard";
    default:
        return "diagnostic";
    }
}
