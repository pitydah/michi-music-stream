/* Host-side tests for the semver parser with pre-release ordering.
 * Compiles the REAL firmware source: components/michi_ota/semver.c
 * (linked from the Makefile) - no reimplementation.
 *
 * Tests cover:
 *   - Valid numeric-only versions (legacy compat)
 *   - Pre-release parsing (rc1, alpha, beta, rc10)
 *   - Pre-release ordering: alpha < beta < rc < final (P0-03)
 *   - OTA RC→final acceptance cases from the plan
 *   - Rejection of malformed strings
 *   - Legacy semver_parse_numeric() still rejects pre-release labels */

#include <stdio.h>
#include <string.h>

#include "semver.h"

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s\n", msg);                                     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

/* ---- helpers ---- */

static bool parse_ok(const char *s, semver_t *out)
{
    if (!semver_parse(s, out)) {
        printf("  FAIL parse ok: '%s'\n", s);
        failures++;
        return false;
    }
    return true;
}

static void expect_parse_bad(const char *s)
{
    semver_t v;
    if (semver_parse(s, &v)) {
        printf("  FAIL parse reject: '%s' accepted\n", s);
        failures++;
    }
}

/* Parse two strings and compare; expected: -1, 0, or 1. */
static void expect_cmp(const char *a, const char *b, int expected,
                       const char *label)
{
    semver_t va, vb;
    if (!semver_parse(a, &va) || !semver_parse(b, &vb)) {
        printf("  FAIL cmp parse error: '%s' vs '%s'\n", a, b);
        failures++;
        return;
    }
    int r = semver_cmp(&va, &vb);
    /* Normalise to -1/0/1. */
    int sign = (r < 0) ? -1 : (r > 0) ? 1 : 0;
    if (sign != expected) {
        printf("  FAIL %s: '%s' vs '%s' got %d want %d\n",
               label, a, b, sign, expected);
        failures++;
    }
}

/* ---- test suites ---- */

static void test_valid_numeric(void)
{
    printf("semver: valid numeric\n");
    semver_t v;
    parse_ok("0.0.0", &v);    CHECK(!v.has_pre, "0.0.0 no pre");
    parse_ok("1.2.3", &v);    CHECK(v.major==1 && v.minor==2 && v.patch==3, "1.2.3");
    parse_ok("0.1.0", &v);    CHECK(!v.has_pre, "0.1.0 no pre");
    parse_ok("10.20.30", &v); CHECK(v.major==10, "10.20.30");
    parse_ok("65535.0.65535", &v); CHECK(v.major==65535, "65535.0.65535");
}

static void test_valid_prerelease(void)
{
    printf("semver: valid pre-release\n");
    semver_t v;
    parse_ok("0.3.0-rc1", &v);
    CHECK(v.has_pre, "0.3.0-rc1 has_pre");
    CHECK(strcmp(v.pre, "rc1") == 0, "0.3.0-rc1 pre=rc1");
    CHECK(v.major==0 && v.minor==3 && v.patch==0, "0.3.0-rc1 numeric");

    parse_ok("1.0.0-alpha", &v);
    CHECK(v.has_pre, "alpha has_pre");
    CHECK(strcmp(v.pre, "alpha") == 0, "alpha label");

    parse_ok("1.0.0-beta", &v);
    CHECK(strcmp(v.pre, "beta") == 0, "beta label");

    parse_ok("2.0.0-rc10", &v);
    CHECK(strcmp(v.pre, "rc10") == 0, "rc10 label");

    parse_ok("0.3.0-rc2", &v);
    CHECK(strcmp(v.pre, "rc2") == 0, "rc2 label");
}

static void test_invalid(void)
{
    printf("semver: invalid\n");
    expect_parse_bad("");           /* empty */
    expect_parse_bad(" 1.2.3");     /* leading whitespace */
    expect_parse_bad("1.2.3 ");     /* trailing whitespace */
    expect_parse_bad("+1.2.3");     /* sign */
    expect_parse_bad("-1.2.3");     /* sign */
    expect_parse_bad("01.2.3");     /* leading zero */
    expect_parse_bad("1.02.3");     /* leading zero mid */
    expect_parse_bad("1.2.3x");     /* trailing junk */
    expect_parse_bad("1.2.3-");     /* empty pre-release label */
    expect_parse_bad("1.2");        /* two parts */
    expect_parse_bad("1.2.3.4");    /* four parts */
    expect_parse_bad("1..3");       /* empty component */
    expect_parse_bad("1.2.");       /* trailing dot */
    expect_parse_bad("65536.0.0");  /* overflow */
    expect_parse_bad("0.0.65536");  /* overflow */
    expect_parse_bad("1.2.3-rc 1"); /* space in label */
    expect_parse_bad("1.2.3+build"); /* build metadata not supported */
}

/* P0-03: OTA version ordering (the critical cases from the plan). */
static void test_ota_ordering(void)
{
    printf("semver: OTA pre-release ordering (P0-03)\n");

    /* RC → final: MUST be accepted (was broken before). */
    expect_cmp("0.3.0-rc1", "0.3.0", -1, "0.3.0-rc1 < 0.3.0 (OTA accept)");

    /* RC → next RC: MUST be accepted. */
    expect_cmp("0.3.0-rc1", "0.3.0-rc2", -1, "0.3.0-rc1 < 0.3.0-rc2 (OTA accept)");

    /* final → RC: MUST be rejected (downgrade). */
    expect_cmp("0.3.0", "0.3.0-rc1", 1, "0.3.0 > 0.3.0-rc1 (OTA reject)");

    /* Downgrade: MUST be rejected. */
    expect_cmp("0.3.0", "0.2.9", 1, "0.3.0 > 0.2.9 (OTA reject downgrade)");

    /* Upgrade: MUST be accepted. */
    expect_cmp("0.3.0", "0.3.1", -1, "0.3.0 < 0.3.1 (OTA accept)");

    /* Alpha < Beta < RC < final */
    expect_cmp("1.0.0-alpha", "1.0.0-beta", -1, "alpha < beta");
    expect_cmp("1.0.0-beta",  "1.0.0-rc1",  -1, "beta < rc1");
    expect_cmp("1.0.0-rc1",   "1.0.0-rc2",  -1, "rc1 < rc2");
    expect_cmp("1.0.0-rc2",   "1.0.0-rc10", -1, "rc2 < rc10 (numeric suffix)");
    expect_cmp("1.0.0-rc10",  "1.0.0",      -1, "rc10 < final");

    /* Equal: must be ==0. */
    expect_cmp("0.3.0",     "0.3.0",     0, "equal numeric");
    expect_cmp("0.3.0-rc1", "0.3.0-rc1", 0, "equal pre-release");

    /* Cross-version: numeric part takes precedence. */
    expect_cmp("0.3.0-rc1", "0.4.0-alpha", -1, "0.3.0-rc1 < 0.4.0-alpha");
    expect_cmp("1.0.0-alpha", "0.9.9", 1, "1.0.0-alpha > 0.9.9");
}

/* Legacy API: semver_parse_numeric still rejects pre-release. */
static void test_legacy_api(void)
{
    printf("semver: legacy numeric-only API\n");
    uint16_t out[3];
    CHECK( semver_parse_numeric("1.2.3", out), "numeric 1.2.3 accepted");
    CHECK(!semver_parse_numeric("1.2.3-rc1", out), "1.2.3-rc1 rejected by numeric");
    CHECK(!semver_parse_numeric("1.2.3-alpha", out), "1.2.3-alpha rejected");
    CHECK(semver_cmp_arr(out, out) == 0, "cmp_arr equal");
    uint16_t a[3] = {1,2,3}, b[3] = {1,2,4};
    CHECK(semver_cmp_arr(a, b) < 0, "1.2.3 < 1.2.4 arr");
    CHECK(semver_cmp_arr(b, a) > 0, "1.2.4 > 1.2.3 arr");
}

int main(void)
{
    test_valid_numeric();
    test_valid_prerelease();
    test_invalid();
    test_ota_ordering();
    test_legacy_api();
    if (failures == 0) {
        printf("PASS test_semver\n");
        return 0;
    }
    printf("FAIL test_semver (%d)\n", failures);
    return 1;
}
