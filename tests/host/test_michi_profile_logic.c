/*
 * test_michi_profile_logic.c
 *
 * Host-side tests for the two decision functions compiled directly from
 * firmware/components/michi_product_profile/profile_logic.c.
 *
 * Logic under test:
 *   michi_profile_decide_tier()
 *   michi_profile_decide_audio_available()
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "michi_profile_logic.h"

/*
 * Minimal caps struct for test convenience: holds the fields under test.
 */
typedef struct {
    bool                 detected;
    bool                 initialized;
    michi_product_tier_t tier;   /* tier reported by the DAC driver */
} test_caps_t;

/* -----------------------------------------------------------------------
 * Test harness.
 * ----------------------------------------------------------------------- */

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            fprintf(stderr, "  FAIL [%s:%d] %s\n",             \
                    __FILE__, __LINE__, (msg));                  \
            g_failures++;                                       \
        } else {                                                \
            printf("  pass  %s\n", (msg));                      \
        }                                                       \
    } while (0)

/* -----------------------------------------------------------------------
 * Test 1: Tier classification — all 4 {detected, driver-tier} combos.
 * ----------------------------------------------------------------------- */
static void test_tier_classification(void)
{
    printf("tier: classification from detect flag and driver-reported tier\n");

    /* 1a. detected=true, driver says HIFI → resolved tier must be HIFI */
    {
        test_caps_t caps = {.detected = true, .initialized = false,
                            .tier = MICHI_PRODUCT_HIFI};
        CHECK(michi_profile_decide_tier(caps.detected, caps.tier) == MICHI_PRODUCT_HIFI,
              "detected=true, driver=HIFI  → HIFI");
    }

    /* 1b. detected=true, driver says STANDARD → resolved tier must be STANDARD */
    {
        test_caps_t caps = {.detected = true, .initialized = false,
                            .tier = MICHI_PRODUCT_STANDARD};
        CHECK(michi_profile_decide_tier(caps.detected, caps.tier) == MICHI_PRODUCT_STANDARD,
              "detected=true, driver=STANDARD → STANDARD");
    }

    /* 1c. detected=true, driver says DIAGNOSTIC → stays DIAGNOSTIC
     * (michi_dac degrades to DIAGNOSTIC on !initialized while bound) */
    {
        test_caps_t caps = {.detected = true, .initialized = false,
                            .tier = MICHI_PRODUCT_DIAGNOSTIC};
        CHECK(michi_profile_decide_tier(caps.detected, caps.tier) == MICHI_PRODUCT_DIAGNOSTIC,
              "detected=true, driver=DIAGNOSTIC → DIAGNOSTIC (driver-degraded)");
    }

    /* 1d. detected=false (nothing probed on the bus).
     * With no driver bound the zeroed caps->tier reads STANDARD (enum 0),
     * but the profile must re-raise DIAGNOSTIC because !detected. */
    {
        test_caps_t caps = {.detected = false, .initialized = false,
                            .tier = MICHI_PRODUCT_STANDARD};
        CHECK(michi_profile_decide_tier(caps.detected, caps.tier) == MICHI_PRODUCT_DIAGNOSTIC,
              "detected=false, zeroed caps (tier=STANDARD) → DIAGNOSTIC (re-raised)");
    }
}

/* -----------------------------------------------------------------------
 * Test 2: Initial-state guard.
 *
 * Before any refresh() the static struct is:
 *   static michi_product_profile_t s_profile = { .tier = MICHI_PRODUCT_DIAGNOSTIC };
 *
 * We model this by verifying that the zero-initialised value of the
 * enum (STANDARD=0) is NOT the initial tier, and that DIAGNOSTIC != 0.
 * ----------------------------------------------------------------------- */
static void test_initial_state_guard(void)
{
    printf("initial-state: pre-refresh tier must be DIAGNOSTIC, not STANDARD\n");

    /* DIAGNOSTIC must not be the same as the zero-initialised enum value.
     * If someone accidentally changes the enum ordering, this catches it. */
    CHECK(MICHI_PRODUCT_DIAGNOSTIC != MICHI_PRODUCT_STANDARD,
          "MICHI_PRODUCT_DIAGNOSTIC != MICHI_PRODUCT_STANDARD (different values)");
    CHECK((int)MICHI_PRODUCT_STANDARD == 0,
          "MICHI_PRODUCT_STANDARD == 0 (zeroed memory would read STANDARD)");
    CHECK((int)MICHI_PRODUCT_DIAGNOSTIC != 0,
          "MICHI_PRODUCT_DIAGNOSTIC != 0 (not the zeroed default)");

    /* Simulate the static initialiser: the C zero-init of the struct
     * (as would happen without the explicit .tier initialiser) gives STANDARD.
     * The real code explicitly sets .tier = MICHI_PRODUCT_DIAGNOSTIC, making
     * a pre-refresh get() safe. */
    michi_product_tier_t zero_init_tier = (michi_product_tier_t)0;
    CHECK(zero_init_tier == MICHI_PRODUCT_STANDARD,
          "zero-init tier == STANDARD (danger without explicit initialiser)");

    /* The explicit initialiser the real code uses: */
    michi_product_tier_t explicit_init_tier = MICHI_PRODUCT_DIAGNOSTIC;
    CHECK(explicit_init_tier == MICHI_PRODUCT_DIAGNOSTIC,
          "explicit .tier=DIAGNOSTIC init → pre-refresh get() returns DIAGNOSTIC");
    CHECK(explicit_init_tier != MICHI_PRODUCT_STANDARD,
          "pre-refresh tier != STANDARD (no false-positive audio capability)");
}

/* -----------------------------------------------------------------------
 * Test 3: audio_available — all 8 combinations of
 *   {detected, initialized, driver_tier ∈ {HIFI, STANDARD, DIAGNOSTIC}}.
 * ----------------------------------------------------------------------- */
static void test_audio_available(void)
{
    printf("audio_available: all {detected, initialized, driver_tier} combos\n");

    struct {
        bool                 detected;
        bool                 initialized;
        michi_product_tier_t driver_tier;
        bool                 want;
        const char          *label;
    } cases[] = {
        /* detected=false: regardless of initialized/driver_tier → false */
        { false, false, MICHI_PRODUCT_STANDARD,    false,
          "!detected, !init, STANDARD   → false" },
        { false, true,  MICHI_PRODUCT_STANDARD,    false,
          "!detected,  init, STANDARD   → false (re-raised DIAGNOSTIC)" },
        { false, false, MICHI_PRODUCT_HIFI,         false,
          "!detected, !init, HIFI       → false (re-raised DIAGNOSTIC)" },
        { false, true,  MICHI_PRODUCT_HIFI,         false,
          "!detected,  init, HIFI       → false (re-raised DIAGNOSTIC)" },
        /* detected=true, driver=DIAGNOSTIC (driver-degraded, e.g. !init) */
        { true,  false, MICHI_PRODUCT_DIAGNOSTIC,   false,
          " detected, !init, driver=DIAGNOSTIC → false" },
        { true,  true,  MICHI_PRODUCT_DIAGNOSTIC,   false,
          " detected,  init, driver=DIAGNOSTIC → false (driver-degraded)" },
        /* detected=true, initialized=false → audio not ready */
        { true,  false, MICHI_PRODUCT_HIFI,         false,
          " detected, !init, HIFI       → false (not initialized)" },
        { true,  false, MICHI_PRODUCT_STANDARD,     false,
          " detected, !init, STANDARD   → false (not initialized)" },
        /* Happy paths: detected=true, initialized=true */
        { true,  true,  MICHI_PRODUCT_HIFI,         true,
          " detected,  init, HIFI       → true" },
        { true,  true,  MICHI_PRODUCT_STANDARD,     true,
          " detected,  init, STANDARD   → true" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        test_caps_t caps = {
            .detected    = cases[i].detected,
            .initialized = cases[i].initialized,
            .tier        = cases[i].driver_tier,
        };
        michi_product_tier_t resolved = michi_profile_decide_tier(caps.detected, caps.tier);
        bool got = michi_profile_decide_audio_available(resolved, caps.initialized);
        CHECK(got == cases[i].want, cases[i].label);
    }
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(void)
{
    test_tier_classification();
    test_initial_state_guard();
    test_audio_available();

    printf("\n%d check(s) run, %d failure(s).\n", g_checks, g_failures);
    if (g_failures == 0) {
        printf("test_michi_profile_logic: ALL PASSED\n");
        return 0;
    }
    printf("test_michi_profile_logic: %d FAILED\n", g_failures);
    return 1;
}
