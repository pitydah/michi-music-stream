#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pure pairing validators (F15: extracted from michi_pairing.c so the
 * host-side tests compile the SAME source - no reimplementation).
 * No ESP-IDF dependencies: unit-testable on the host. */

/* @return true when pin is exactly 6 ASCII digits (MICHI_PAIRING_PIN_LEN). */
bool michi_pairing_pin_valid(const char *pin);

/* @return true when id is a UUID v4 string: 36 chars, the canonical
 *         8-4-4-4-12 lowercase-hex grouping (e.g.
 *         "550e8400-e29b-41d4-a716-446655440000"). */
bool michi_pairing_uuid_valid(const char *id);

/* Constant-time byte comparison: loops over ALL n bytes (no early return
 * on data, no branch on the contents) accumulating into a volatile
 * accumulator, so a mismatch anywhere yields false with data-independent
 * timing. Returns true only when a == b over the whole range. */
bool michi_pairing_token_matches(const uint8_t *a, const uint8_t *b, size_t n);

#ifdef __cplusplus
}
#endif
