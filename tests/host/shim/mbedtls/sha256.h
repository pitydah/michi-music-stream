#pragma once
/* Shim for host-side tests: one-shot mbedtls_sha256() backed by a
 * compact, standard SHA-256 (FIPS 180-4) implementation - a test double
 * for the mbedTLS call the pairing component uses in production. The
 * pairing host tests cross-check it against known-answer vectors and
 * against python hashlib digests. TEST-ONLY: never compiled into
 * firmware (the firmware links real mbedTLS). */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-shot SHA-256: mbedtls_sha256(data, len, output, 0). Returns 0 on
 * success (mbedTLS contract). */
int mbedtls_sha256(const unsigned char *input, size_t ilen,
                   unsigned char output[32], int is224);

#ifdef __cplusplus
}
#endif
