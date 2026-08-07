#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Strict semver (x.y.z numeric only; used for downgrade prevention).
 * Shared with the host-side tests (tests/host/test_semver.c compiles the
 * SAME semver.c - no reimplementation, no duplication).
 *
 * Mirrors the signer's validation: every component is digit-only (no
 * leading whitespace, no sign, no leading zeros - '01.2.3' is rejected)
 * and fits in uint16_t. */

/* @return true when s is a valid strict semver; out receives the parts. */
bool semver_parse(const char *s, uint16_t out[3]);

/* @return -1/0/1 comparing a vs b component-wise (semver order). */
int semver_cmp(const uint16_t a[3], const uint16_t b[3]);

#ifdef __cplusplus
}
#endif
