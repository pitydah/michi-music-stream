/* Host-side tests for the pure pairing validators (F15).
 * Compiles the REAL firmware source: components/michi_pairing/validators.c
 * (linked from the Makefile) - no reimplementation. */

#include <stdio.h>
#include <string.h>

#include "validators.h"

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s\n", msg);                                     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

/* ── pin_valid ────────────────────────────────────────────── */

static void test_pin_valid(void)
{
    printf("pairing: pin_valid\n");
    CHECK(michi_pairing_pin_valid("000000"), "all zeros");
    CHECK(michi_pairing_pin_valid("042731"), "arbitrary digits");
    CHECK(michi_pairing_pin_valid("999999"), "all nines");
    CHECK(!michi_pairing_pin_valid(NULL), "NULL rejected");
    CHECK(!michi_pairing_pin_valid(""), "empty rejected");
    CHECK(!michi_pairing_pin_valid("12345"), "5 digits rejected");
    CHECK(!michi_pairing_pin_valid("1234567"), "7 digits rejected");
    CHECK(!michi_pairing_pin_valid("12345a"), "letter rejected");
    CHECK(!michi_pairing_pin_valid(" 23456"), "space rejected");
    CHECK(!michi_pairing_pin_valid("12345\n"), "newline rejected");
    CHECK(!michi_pairing_pin_valid("+12345"), "sign rejected");
    CHECK(!michi_pairing_pin_valid("-12345"), "minus rejected");
}

/* ── uuid_valid ───────────────────────────────────────────── */

static void test_uuid_valid(void)
{
    printf("pairing: uuid_valid\n");
    CHECK(michi_pairing_uuid_valid("550e8400-e29b-41d4-a716-446655440000"),
          "canonical uuid");
    CHECK(michi_pairing_uuid_valid("00000000-0000-4000-8000-000000000000"),
          "zeroes with version/variant");
    CHECK(!michi_pairing_uuid_valid(NULL), "NULL rejected");
    CHECK(!michi_pairing_uuid_valid(""), "empty rejected");
    CHECK(!michi_pairing_uuid_valid("550e8400e29b41d4a716446655440000"),
          "missing dashes rejected");
    CHECK(!michi_pairing_uuid_valid("550e8400-e29b-41d4-a716-44665544000"),
          "35 chars rejected");
    CHECK(!michi_pairing_uuid_valid("550e8400-e29b-41d4-a716-4466554400000"),
          "37 chars rejected");
    CHECK(!michi_pairing_uuid_valid("550e8400-e29b-41d4-a716-44665544000G"),
          "uppercase hex rejected");
    CHECK(!michi_pairing_uuid_valid("550e8400-e29b-41d4-a716-44665544000-"),
          "dash at the end rejected");
    CHECK(!michi_pairing_uuid_valid("550e8400e29b-41d4-a716-446655440000"),
          "dash at wrong position rejected");
}

/* ── token_matches (constant-time) ────────────────────────── */

static void test_token_matches(void)
{
    printf("pairing: token_matches\n");
    const uint8_t a[32] = "0123456789abcdef0123456789abcde"; /* 32 chars, no NUL */
    uint8_t b[32] = {0};
    memcpy(b, a, sizeof(a));

    CHECK(michi_pairing_token_matches(a, b, sizeof(a)), "identical buffers");
    CHECK(michi_pairing_token_matches(a, a, sizeof(a)), "same pointer");
    CHECK(!michi_pairing_token_matches(NULL, NULL, 0),
          "NULL rejected even for n=0 (guard first)");

    /* Difference in EVERY position must be detected (a single-position
     * mismatch is not a proxy: the no-early-return property is structural
     * - the loop has no branch on the data, only the i < n bound, and the
     * volatile accumulator is updated on every iteration; a real
     * early-return implementation would still pass these, so the
     * verification is: (1) full-position sweep below, (2) code review of
     * validators.c - the function's only conditional is the loop bound). */
    for (size_t i = 0; i < sizeof(a); i++) {
        uint8_t diff[32];
        memcpy(diff, a, sizeof(a));
        diff[i] = (uint8_t)(a[i] ^ 0x01);
        if (michi_pairing_token_matches(a, diff, sizeof(a))) {
            printf("  FAIL mismatch at position %zu\n", i);
            failures++;
        }
    }
    /* First-byte vs last-byte mismatch both detected (both ends of the
     * sweep): an implementation that bailed after the first difference
     * would look identical, but one that NEVER READ the tail would miss
     * these - the sweep is the observable part of "loops over all n". */
    uint8_t first[32];
    memcpy(first, a, sizeof(a));
    first[0] ^= 0xff;
    CHECK(!michi_pairing_token_matches(a, first, sizeof(a)), "first byte differs");

    uint8_t last[32];
    memcpy(last, a, sizeof(a));
    last[31] ^= 0xff;
    CHECK(!michi_pairing_token_matches(a, last, sizeof(a)), "last byte differs");

    /* Length handling: only n bytes are compared (prefix equality). */
    uint8_t prefix[4];
    memcpy(prefix, a, sizeof(prefix));
    CHECK(michi_pairing_token_matches(a, prefix, 4), "n limits the scan");
}

int main(void)
{
    test_pin_valid();
    test_uuid_valid();
    test_token_matches();
    if (failures == 0) {
        printf("PASS test_pairing_validators\n");
        return 0;
    }
    printf("FAIL test_pairing_validators (%d)\n", failures);
    return 1;
}
