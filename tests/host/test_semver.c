/* Host-side tests for the strict semver parser (F15).
 * Compiles the REAL firmware source: components/michi_ota/semver.c
 * (linked from the Makefile) - no reimplementation. */

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

static void expect_parse_ok(const char *s, uint16_t a, uint16_t b, uint16_t c)
{
    uint16_t out[3] = {0xffff, 0xffff, 0xffff};
    if (!semver_parse(s, out)) {
        printf("  FAIL parse ok: '%s'\n", s);
        failures++;
        return;
    }
    if (out[0] != a || out[1] != b || out[2] != c) {
        printf("  FAIL parse values: '%s' -> %u.%u.%u (want %u.%u.%u)\n",
               s, (unsigned)out[0], (unsigned)out[1], (unsigned)out[2],
               (unsigned)a, (unsigned)b, (unsigned)c);
        failures++;
    }
}

static void expect_parse_bad(const char *s)
{
    uint16_t out[3] = {0};
    if (semver_parse(s, out)) {
        printf("  FAIL parse reject: '%s' accepted\n", s);
        failures++;
    }
}

static void test_valid(void)
{
    printf("semver: valid\n");
    expect_parse_ok("0.0.0", 0, 0, 0);
    expect_parse_ok("1.2.3", 1, 2, 3);
    expect_parse_ok("0.1.0", 0, 1, 0);
    expect_parse_ok("10.20.30", 10, 20, 30);
    expect_parse_ok("65535.0.65535", 65535, 0, 65535);
    expect_parse_ok("12.34.56", 12, 34, 56);
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
    expect_parse_bad("1.02.3");     /* leading zero (mid component) */
    expect_parse_bad("1.2.3x");     /* trailing junk */
    expect_parse_bad("1.2.3-rc1");  /* prerelease: not supported */
    expect_parse_bad("1.2");        /* two parts */
    expect_parse_bad("1.2.3.4");    /* four parts */
    expect_parse_bad("1..3");       /* empty component */
    expect_parse_bad("1.2.");       /* trailing dot */
    expect_parse_bad("65536.0.0");  /* overflow > uint16 */
    expect_parse_bad("0.0.65536");  /* overflow > uint16 */
}

static void test_compare(void)
{
    printf("semver: compare\n");
    const uint16_t a[3] = {1, 2, 3};
    const uint16_t b[3] = {1, 2, 4};
    const uint16_t c[3] = {2, 0, 0};
    const uint16_t d[3] = {1, 9, 9};
    const uint16_t e[3] = {0, 0, 1};
    const uint16_t f[3] = {0, 0, 0};
    const uint16_t z[3] = {0, 0, 0};
    CHECK(semver_cmp(a, b) < 0, "1.2.3 < 1.2.4");
    CHECK(semver_cmp(b, a) > 0, "1.2.4 > 1.2.3");
    CHECK(semver_cmp(a, a) == 0, "equal");
    CHECK(semver_cmp(c, d) > 0, "2.0.0 > 1.9.9");
    CHECK(semver_cmp(d, c) < 0, "1.9.9 < 2.0.0");
    CHECK(semver_cmp(e, f) > 0, "0.0.1 > 0.0.0");
    CHECK(semver_cmp(f, e) < 0, "0.0.0 < 0.0.1");
    CHECK(semver_cmp(z, z) == 0, "0.0.0 == 0.0.0");
}

int main(void)
{
    test_valid();
    test_invalid();
    test_compare();
    if (failures == 0) {
        printf("PASS test_semver\n");
        return 0;
    }
    printf("FAIL test_semver (%d)\n", failures);
    return 1;
}
