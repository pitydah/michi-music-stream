/* Semver parser with pre-release tag ordering.
 *
 * Numeric x.y.z must be digit-only, no leading zeros, fit in uint16_t.
 * Pre-release labels are optional ASCII identifiers after '-'.
 * A version without a pre-release label is GREATER than any version with
 * the same x.y.z and a pre-release label (SemVer 2.0 §11.3).
 *
 * Pre-release segment ordering within equal x.y.z:
 *   - Segments are split by '.'.
 *   - Numeric segments compare numerically; identifier segments compare
 *     lexicographically (SemVer 2.0 §11.4).
 *   - alpha < beta < rc (lex); rc1 < rc2 < rc10 (numeric suffix).
 *
 * Shared with host-side tests: test_semver.c compiles THIS file directly. */

#include "semver.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------ */

/* Parse one unsigned decimal integer from *pp; advances *pp.
 * Returns false on overflow (> 65535), empty, or non-digit start. */
static bool parse_uint16(const char **pp, uint16_t *out)
{
    const char *p = *pp;
    if (*p == '\0' || !isdigit((unsigned char)*p)) {
        return false;
    }
    if (*p == '0' && isdigit((unsigned char)p[1])) {
        return false; /* leading zero: "01" rejected */
    }
    unsigned long v = 0;
    while (isdigit((unsigned char)*p)) {
        v = v * 10u + (unsigned long)(*p - '0');
        if (v > 65535u) {
            return false;
        }
        p++;
    }
    *out = (uint16_t)v;
    *pp  = p;
    return true;
}

/* Compare two pre-release segments per SemVer 2.0 §11.4 with an extension
 * for alphanumeric labels that share a common prefix followed by a numeric
 * suffix (e.g. "rc1" < "rc2" < "rc10").
 *
 * Rules (in order):
 *   1. Both purely numeric → numeric comparison.
 *   2. Both share the same alphabetic prefix + numeric suffix →
 *      compare prefix lex, then suffix numerically.
 *   3. Otherwise → lex comparison of the full segment. */
static int cmp_pre_segment(const char *a, size_t alen,
                           const char *b, size_t blen)
{
    /* Detect purely numeric segments. */
    bool a_num = (alen > 0), b_num = (blen > 0);
    for (size_t i = 0; i < alen; i++) {
        if (!isdigit((unsigned char)a[i])) { a_num = false; break; }
    }
    for (size_t i = 0; i < blen; i++) {
        if (!isdigit((unsigned char)b[i])) { b_num = false; break; }
    }

    if (a_num && b_num) {
        unsigned long av = 0, bv = 0;
        for (size_t i = 0; i < alen; i++) av = av * 10u + (unsigned long)(a[i] - '0');
        for (size_t i = 0; i < blen; i++) bv = bv * 10u + (unsigned long)(b[i] - '0');
        return (av < bv) ? -1 : (av > bv) ? 1 : 0;
    }

    /* Find where the trailing numeric suffix begins in each segment. */
    size_t a_alpha_end = alen;
    while (a_alpha_end > 0 && isdigit((unsigned char)a[a_alpha_end - 1])) {
        a_alpha_end--;
    }
    size_t b_alpha_end = blen;
    while (b_alpha_end > 0 && isdigit((unsigned char)b[b_alpha_end - 1])) {
        b_alpha_end--;
    }

    /* If both have the same alphabetic prefix, compare numeric suffixes. */
    if (a_alpha_end == b_alpha_end && a_alpha_end > 0 &&
        strncmp(a, b, a_alpha_end) == 0) {
        /* Same alpha prefix: compare numeric suffixes. */
        const char *a_sfx = a + a_alpha_end;
        const char *b_sfx = b + b_alpha_end;
        size_t a_slen = alen - a_alpha_end;
        size_t b_slen = blen - b_alpha_end;
        /* No suffix on one → treat suffix as 0. */
        unsigned long av = 0, bv = 0;
        for (size_t i = 0; i < a_slen; i++) av = av * 10u + (unsigned long)(a_sfx[i] - '0');
        for (size_t i = 0; i < b_slen; i++) bv = bv * 10u + (unsigned long)(b_sfx[i] - '0');
        return (av < bv) ? -1 : (av > bv) ? 1 : 0;
    }

    /* Fallback: lex comparison. */
    size_t minlen = alen < blen ? alen : blen;
    int r = strncmp(a, b, minlen);
    if (r != 0) return r;
    return (alen < blen) ? -1 : (alen > blen) ? 1 : 0;
}


/* Compare two pre-release label strings (dot-separated segments).
 * Empty label means "no pre-release" and is treated as GREATER. */
static int cmp_pre_labels(const char *a, bool a_has,
                          const char *b, bool b_has)
{
    if (!a_has && !b_has) return 0;
    if (!a_has && b_has)  return 1;   /* final > pre-release */
    if (a_has  && !b_has) return -1;  /* pre-release < final */

    /* Both have pre-release labels: compare segment by segment. */
    const char *pa = a;
    const char *pb = b;
    for (;;) {
        /* Find end of current segment in a. */
        const char *ea = pa;
        while (*ea != '\0' && *ea != '.') ea++;
        const char *eb = pb;
        while (*eb != '\0' && *eb != '.') eb++;

        int r = cmp_pre_segment(pa, (size_t)(ea - pa),
                                pb, (size_t)(eb - pb));
        if (r != 0) return r;

        /* Advance past the segment. */
        pa = (*ea == '.') ? ea + 1 : ea;
        pb = (*eb == '.') ? eb + 1 : eb;

        /* If both exhausted: equal. */
        if (*pa == '\0' && *pb == '\0') return 0;
        /* Longer has more segments → greater. */
        if (*pa == '\0') return -1;
        if (*pb == '\0') return 1;
    }
}

/* ---------------------------------------------------------------------------
 * Public API — full semver_t
 * ------------------------------------------------------------------------ */

bool semver_parse(const char *s, semver_t *out)
{
    if (s == NULL || out == NULL) {
        return false;
    }

    const char *p = s;
    semver_t v = {0};

    /* major.minor.patch */
    if (!parse_uint16(&p, &v.major)) return false;
    if (*p != '.') return false;
    p++;
    if (!parse_uint16(&p, &v.minor)) return false;
    if (*p != '.') return false;
    p++;
    if (!parse_uint16(&p, &v.patch)) return false;

    /* Optional pre-release label: -<identifier> */
    if (*p == '-') {
        p++; /* skip '-' */
        if (*p == '\0') return false; /* empty label rejected */
        /* Validate: only alphanumeric and dot allowed in label. */
        size_t llen = 0;
        const char *lstart = p;
        while (*p != '\0') {
            if (!isalnum((unsigned char)*p) && *p != '.') {
                return false; /* invalid character */
            }
            p++;
            llen++;
        }
        if (llen == 0 || llen >= SEMVER_PRE_MAX) return false;
        v.has_pre = true;
        memcpy(v.pre, lstart, llen);
        v.pre[llen] = '\0';
    } else if (*p != '\0') {
        return false; /* trailing junk */
    }

    *out = v;
    return true;
}

int semver_cmp(const semver_t *a, const semver_t *b)
{
    if (a->major != b->major) return (a->major < b->major) ? -1 : 1;
    if (a->minor != b->minor) return (a->minor < b->minor) ? -1 : 1;
    if (a->patch != b->patch) return (a->patch < b->patch) ? -1 : 1;
    return cmp_pre_labels(a->pre, a->has_pre, b->pre, b->has_pre);
}

/* ---------------------------------------------------------------------------
 * Legacy numeric-only array API (backward compat for non-OTA callers)
 * ------------------------------------------------------------------------ */

bool semver_parse_numeric(const char *s, uint16_t out[3])
{
    semver_t v;
    if (!semver_parse(s, &v)) return false;
    if (v.has_pre) return false; /* reject pre-release in strict mode */
    out[0] = v.major;
    out[1] = v.minor;
    out[2] = v.patch;
    return true;
}

int semver_cmp_arr(const uint16_t a[3], const uint16_t b[3])
{
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) return (a[i] < b[i]) ? -1 : 1;
    }
    return 0;
}
