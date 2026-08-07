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

/* ── id_valid ─────────────────────────────────────────────── */

static void test_id_valid(void)
{
    printf("pairing: id_valid\n");
    CHECK(michi_pairing_id_valid("a"), "single alnum");
    CHECK(michi_pairing_id_valid("A"), "single upper");
    CHECK(michi_pairing_id_valid("1"), "single digit");
    CHECK(michi_pairing_id_valid("abc-123"), "alnum + dash");
    CHECK(michi_pairing_id_valid("0"), "digit");
    {
        char long_id[MICHI_PAIRING_ID_MAX + 1];
        memset(long_id, 'x', MICHI_PAIRING_ID_MAX);
        long_id[MICHI_PAIRING_ID_MAX] = '\0';
        CHECK(michi_pairing_id_valid(long_id), "max length 31");
        char long_dash[MICHI_PAIRING_ID_MAX + 1];
        memset(long_dash, 'x', MICHI_PAIRING_ID_MAX - 2);
        long_dash[MICHI_PAIRING_ID_MAX - 2] = '-';
        long_dash[MICHI_PAIRING_ID_MAX - 1] = 'y';
        long_dash[MICHI_PAIRING_ID_MAX] = '\0';
        CHECK(michi_pairing_id_valid(long_dash), "max length with dash");
    }
    CHECK(!michi_pairing_id_valid(NULL), "NULL rejected");
    CHECK(!michi_pairing_id_valid(""), "empty rejected");
    CHECK(!michi_pairing_id_valid("_"), "underscore rejected");
    CHECK(!michi_pairing_id_valid("abc_123"), "underscore rejected");
    CHECK(!michi_pairing_id_valid("abc 123"), "space rejected");
    CHECK(!michi_pairing_id_valid("ab.c"), "dot rejected");
    CHECK(!michi_pairing_id_valid("a/b"), "slash rejected");
    CHECK(!michi_pairing_id_valid("a\tb"), "tab rejected");
    {
        char bad_control[3] = {'a', 0x01, '\0'};
        CHECK(!michi_pairing_id_valid(bad_control), "control char rejected");
    }
    {
        char too_long[MICHI_PAIRING_ID_MAX + 2];
        memset(too_long, 'x', MICHI_PAIRING_ID_MAX + 1);
        too_long[MICHI_PAIRING_ID_MAX + 1] = '\0';
        CHECK(!michi_pairing_id_valid(too_long), "32 chars rejected");
    }
}

/* ── hex helpers ──────────────────────────────────────────── */

static void test_hex_decode(void)
{
    printf("pairing: hex decode\n");
    uint8_t out[32] = {0};
    const char *tok_hex = "0123456789abcdef";
    const char *tok_upper = "0123456789ABCDEF";
    const char *tok_odd = "z123456789abcdef";

    CHECK(michi_pairing_hex_decode(tok_hex, 16, out, 8), "16 hex -> 8 bytes");
    CHECK(out[0] == 0x01 && out[7] == 0xef, "decoded values");
    CHECK(michi_pairing_hex_decode(tok_upper, 16, out, 8), "uppercase hex");
    CHECK(!michi_pairing_hex_decode(tok_odd, 16, out, 8), "non-hex rejected");
    CHECK(!michi_pairing_hex_decode(tok_hex, 15, out, 8), "wrong src_len rejected");
    CHECK(!michi_pairing_hex_decode(tok_hex, 17, out, 8), "wrong src_len rejected");
    CHECK(!michi_pairing_hex_decode(NULL, 16, out, 8), "NULL src rejected");
    CHECK(!michi_pairing_hex_decode(tok_hex, 16, NULL, 8), "NULL dst rejected");

    /* 64-hex token (32 bytes) - the pairing token shape. */
    char full[65];
    memset(full, 'a', 64);
    full[64] = '\0';
    CHECK(michi_pairing_hex_decode(full, 64, out, 32), "64 hex accepted");
    char short_hex[64];
    memset(short_hex, 'a', 63);
    short_hex[63] = '\0';
    CHECK(!michi_pairing_hex_decode(short_hex, 63, out, 32), "63 hex rejected");
    full[63] = 'g';
    CHECK(!michi_pairing_hex_decode(full, 64, out, 32), "'g' rejected");

    /* hex_val direct. */
    CHECK(michi_pairing_hex_val('0') == 0 && michi_pairing_hex_val('f') == 15,
          "hex_val bounds");
    CHECK(michi_pairing_hex_val('F') == 15, "hex_val upper");
    CHECK(michi_pairing_hex_val('x') == 0xff, "hex_val non-hex sentinel");
}

static void test_hex_roundtrip(void)
{
    printf("pairing: hex roundtrip\n");
    const uint8_t raw[8] = {0x00, 0x11, 0xab, 0xcd, 0xef, 0x99, 0x7f, 0x80};
    char hex[17];
    uint8_t back[8] = {0};
    michi_pairing_hex_encode(raw, sizeof(raw), hex);
    CHECK(strcmp(hex, "0011abcdef997f80") == 0, "encode output");
    CHECK(michi_pairing_hex_decode(hex, strlen(hex), back, sizeof(back)),
          "roundtrip decode");
    CHECK(memcmp(raw, back, sizeof(raw)) == 0, "roundtrip equality");
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
    CHECK(michi_pairing_token_matches(NULL, NULL, 0), "n=0 is vacuously equal");

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
    test_id_valid();
    test_hex_decode();
    test_hex_roundtrip();
    test_token_matches();
    if (failures == 0) {
        printf("PASS test_pairing_validators\n");
        return 0;
    }
    printf("FAIL test_pairing_validators (%d)\n", failures);
    return 1;
}
