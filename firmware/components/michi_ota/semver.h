#pragma once

/* Semver parser with pre-release tag ordering.
 *
 * Supports the full ordering required for RC → final OTA:
 *   0.3.0-alpha < 0.3.0-beta < 0.3.0-rc1 < 0.3.0-rc2 < 0.3.0 < 0.3.1
 *
 * Numeric components (x.y.z) must be digit-only, no leading zeros, fit
 * in uint16_t.  Pre-release labels (-alpha, -beta, -rc<N>) are optional;
 * a version WITHOUT a pre-release label is greater than any version WITH
 * the same x.y.z and a pre-release label (SemVer 2.0 §11.3).
 *
 * Pre-release ordering (alphabetically / numerically per segment):
 *   alpha < beta < rc  (by lexicographic segment comparison)
 *   rc1 < rc2 < rc10   (numeric suffix within the same label)
 *
 * The module is shared with host-side tests (test_semver.c compiles the
 * same semver.c - no reimplementation). */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum length for a pre-release label string (e.g. "rc1", "alpha"). */
#define SEMVER_PRE_MAX 32

typedef struct {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
    bool     has_pre;                  /* true when a -label was parsed */
    char     pre[SEMVER_PRE_MAX];      /* pre-release label, NUL-terminated */
} semver_t;

/**
 * @brief Parse a semver string (with optional pre-release label) into
 *        a semver_t.
 *
 * @param s    Input string, e.g. "0.3.0-rc1" or "1.2.3".
 * @param out  Receives the parsed result on success.
 * @return true on success; false when the string is malformed.
 */
bool semver_parse(const char *s, semver_t *out);

/**
 * @brief Compare two semver values per SemVer 2.0 ordering.
 *
 * @return  < 0 if a < b
 *            0 if a == b
 *          > 0 if a > b
 */
int  semver_cmp(const semver_t *a, const semver_t *b);

/* -------------------------------------------------------------------------
 * Legacy three-element array API (used by callers that only compare
 * numeric versions).  Pre-release tags are NOT supported here; a string
 * containing '-' is rejected.  Call semver_parse() + semver_cmp() for
 * full pre-release ordering.
 * ---------------------------------------------------------------------- */

/** @return true when s is a valid numeric-only semver; out[3] receives
 *          {major, minor, patch}. Rejects any pre-release label. */
bool semver_parse_numeric(const char *s, uint16_t out[3]);

/** @return -1/0/1 comparing a vs b component-wise. */
int  semver_cmp_arr(const uint16_t a[3], const uint16_t b[3]);

#ifdef __cplusplus
}
#endif
