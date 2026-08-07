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

/* Max controller id length (single source of truth: the pairing
 * component and the host tests share it). */
#define MICHI_PAIRING_ID_MAX 31

/* @return true when id is alphanumeric + '-' (1..MICHI_PAIRING_ID_MAX). */
bool michi_pairing_id_valid(const char *id);

/* @return the nibble value of a hex char, 0xff for non-hex. */
uint8_t michi_pairing_hex_val(char c);

/* @return true when src is src_len hex chars and the decoded bytes fit dst
 *         (exact length contract: src_len == 2 * dst_len). */
bool michi_pairing_hex_decode(const char *src, size_t src_len, uint8_t *dst,
                              size_t dst_len);

/* Lowercase-hex encode src into dst (writes 2*len chars + NUL). */
void michi_pairing_hex_encode(const uint8_t *src, size_t len, char *dst);

/* Constant-time byte comparison: loops over ALL n bytes (no early return
 * on data, no branch on the contents) accumulating into a volatile
 * accumulator, so a mismatch anywhere yields false with data-independent
 * timing. Returns true only when a == b over the whole range. */
bool michi_pairing_token_matches(const uint8_t *a, const uint8_t *b, size_t n);

#ifdef __cplusplus
}
#endif
