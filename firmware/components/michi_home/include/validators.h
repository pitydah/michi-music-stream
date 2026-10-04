#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* @return true when id is a UUID v4 string: 36 chars, the canonical
 *         8-4-4-4-12 lowercase-hex grouping (e.g.
 *         "550e8400-e29b-41d4-a716-446655440000"). */
bool michi_uuid_valid(const char *id);

/* Constant-time byte comparison. */
bool michi_token_matches(const uint8_t *a, const uint8_t *b, size_t n);

/* Backwards-compatibility aliases */
#define michi_pairing_uuid_valid michi_uuid_valid
#define michi_pairing_token_matches michi_token_matches

#ifdef __cplusplus
}
#endif
