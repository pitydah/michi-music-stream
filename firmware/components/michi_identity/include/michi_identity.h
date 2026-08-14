#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Persistent device identity (MS-04): Ed25519 (RFC 8032) + BLAKE3, scheme
 * "ed25519-blake3-v1" - byte-compatible with the Michi Link michi-identity
 * crate (see firmware/components/michi_identity/README.md).
 *
 * Contract (convergence spec, sections 2.1/4 and MS-04):
 * - The 32-byte Ed25519 seed is generated ONCE with esp_fill_random
 *   (hardware RNG) and persisted in NVS; it is never regenerated
 *   implicitly.
 * - michi_id = base64url-nopad(blake3(public_key_bytes)) - 43 chars.
 * - Wire encoding is base64url WITHOUT padding everywhere (public key
 *   43 chars, signature 86 chars). No '+'/'/'=' on the wire, ever.
 * - States: UNINITIALIZED (no store touched yet), READY (keys loaded or
 *   generated + persisted), CORRUPT (store present but invalid).
 * - A CORRUPT identity requires an explicit factory reset
 *   (michi_identity_factory_reset): the component NEVER silently
 *   regenerates over a corrupt store.
 * - Zero secrets in logs: the seed and secret key are never logged;
 *   michi_id (public) is the only identity value that may appear.
 *
 * Threading: michi_identity_init()/factory_reset() are initialization
 * operations (boot / explicit factory reset path); the getters, sign and
 * verify only read immutable RAM state. The component does not create
 * tasks or timers.
 */

/** Number of raw bytes in an Ed25519 seed / public key. */
#define MICHI_IDENTITY_KEY_BYTES 32
/** Number of raw bytes in an Ed25519 signature. */
#define MICHI_IDENTITY_SIGNATURE_BYTES 64
/** Length of the base64url-nopad michi_id (43 chars + NUL). */
#define MICHI_IDENTITY_MICHI_ID_LEN 44
/** Length of the base64url-nopad public key (43 chars + NUL). */
#define MICHI_IDENTITY_PUBLIC_KEY_B64_LEN 44
/** Length of the base64url-nopad signature (86 chars + NUL). */
#define MICHI_IDENTITY_SIGNATURE_B64_LEN 87
/** Canonical identity scheme (contract v1). */
#define MICHI_IDENTITY_SCHEME "ed25519-blake3-v1"

/**
 * @brief Identity subsystem states.
 */
typedef enum {
    MICHI_IDENTITY_UNINITIALIZED = 0, /*!< Store untouched this boot */
    MICHI_IDENTITY_READY,             /*!< Keys available */
    MICHI_IDENTITY_CORRUPT,           /*!< Store present but invalid; factory
                                           reset required */
} michi_identity_state_t;

/**
 * @brief Initialize the identity: load the persisted seed or, ONLY when
 *        the store is empty (first boot), generate a seed with
 *        esp_fill_random and persist it exactly once.
 *
 * State transitions (sticky):
 *   UNINITIALIZED --store empty--> generate + persist -> READY
 *   UNINITIALIZED --store valid---> READY
 *   UNINITIALIZED --store invalid/read error--> CORRUPT
 *   READY         -> no-op (ESP_OK)
 *   CORRUPT       -> ESP_ERR_INVALID_STATE (factory reset required)
 *
 * Logs the derived michi_id (public) on success; never logs the seed or
 * secret key. The NVS store lives in namespace "michi_identity".
 *
 * @return ESP_OK; ESP_ERR_INVALID_STATE when CORRUPT; NVS/esp_fill_random
 *         errors propagated unchanged (state becomes CORRUPT when the
 *         store exists but cannot be loaded; a failed first-boot persist
 *         keeps the state UNINITIALIZED so the boot can retry).
 */
esp_err_t michi_identity_init(void);

/**
 * @brief Current identity state.
 */
michi_identity_state_t michi_identity_get_state(void);

/**
 * @brief Explicit factory reset: erase the persisted identity and wipe
 *        the in-RAM keys. The state returns to UNINITIALIZED; the caller
 *        then calls michi_identity_init() to mint a fresh identity.
 *
 * This is the ONLY recovery path for CORRUPT.
 *
 * @return ESP_OK; NVS erase errors propagated unchanged.
 */
esp_err_t michi_identity_factory_reset(void);

/**
 * @brief Copy the raw 32-byte Ed25519 public key.
 *
 * @param out Buffer of MICHI_IDENTITY_KEY_BYTES bytes.
 * @return ESP_OK; ESP_ERR_INVALID_STATE unless READY.
 */
esp_err_t michi_identity_public_key(uint8_t out[MICHI_IDENTITY_KEY_BYTES]);

/**
 * @brief Copy the canonical michi_id (43 base64url chars + NUL).
 *
 * @param out     Buffer (>= MICHI_IDENTITY_MICHI_ID_LEN bytes).
 * @param out_len Size of out.
 * @return ESP_OK; ESP_ERR_INVALID_STATE unless READY;
 *         ESP_ERR_INVALID_SIZE when the buffer is too small.
 */
esp_err_t michi_identity_michi_id(char *out, size_t out_len);

/**
 * @brief Sign a payload with Ed25519 (RFC 8032), raw signature bytes.
 *
 * @param msg     Message bytes (may be NULL only when msg_len == 0).
 * @param msg_len Message length.
 * @param sig     Buffer of MICHI_IDENTITY_SIGNATURE_BYTES bytes.
 * @return ESP_OK; ESP_ERR_INVALID_STATE unless READY;
 *         ESP_ERR_INVALID_ARG on NULL pointers.
 */
esp_err_t michi_identity_sign(const uint8_t *msg, size_t msg_len,
                              uint8_t sig[MICHI_IDENTITY_SIGNATURE_BYTES]);

/**
 * @brief Verify an Ed25519 (RFC 8032) signature. Stateless: available in
 *        any state (used to validate peer challenges during pairing).
 *
 * Strict verification (rejects malleable S and off-curve points), as
 * required for interop with ed25519-dalek signatures.
 *
 * @return true when the signature is valid for msg under pk; false
 *         otherwise (also false for NULL arguments).
 */
bool michi_identity_verify(const uint8_t *msg, size_t msg_len,
                           const uint8_t sig[MICHI_IDENTITY_SIGNATURE_BYTES],
                           const uint8_t pk[MICHI_IDENTITY_KEY_BYTES]);

/**
 * @brief Derive the canonical michi_id from a raw 32-byte public key:
 *        base64url-nopad(blake3(public_key_bytes)), 43 chars + NUL.
 *
 * Stateless and available in any state (the pairing flow checks that a
 * peer's michi_id corresponds to its public_key with this function).
 * The caller must ensure the key is a valid Ed25519 public key (the Rust
 * crate applies the same precondition via VerifyingKey::from_bytes).
 *
 * @param pk      Raw 32-byte public key.
 * @param out     Buffer (>= MICHI_IDENTITY_MICHI_ID_LEN bytes).
 * @param out_len Size of out.
 * @return ESP_OK; ESP_ERR_INVALID_ARG on NULL pk;
 *         ESP_ERR_INVALID_SIZE when the buffer is too small.
 */
esp_err_t michi_identity_derive_michi_id(const uint8_t pk[MICHI_IDENTITY_KEY_BYTES],
                                         char *out, size_t out_len);

/**
 * @brief Encode bytes as base64url WITHOUT padding (RFC 4648 URL-safe
 *        alphabet, no '='). Canonical wire encoding of the contract.
 *
 * @return ESP_OK; ESP_ERR_INVALID_ARG on NULL/zero-length input;
 *         ESP_ERR_INVALID_SIZE when the output buffer is too small
 *         (needs 4*ceil(in_len/3)+1 bytes including NUL).
 */
esp_err_t michi_identity_base64url_encode(const uint8_t *in, size_t in_len,
                                          char *out, size_t out_len);

/**
 * @brief Decode base64url WITHOUT padding (strict: rejects '=', '+',
 *        '/' and invalid lengths).
 *
 * @param out     Output buffer.
 * @param out_cap Capacity of out.
 * @param out_len Receives the decoded length.
 * @return ESP_OK; ESP_ERR_INVALID_ARG on malformed input;
 *         ESP_ERR_INVALID_SIZE when the output buffer is too small.
 */
esp_err_t michi_identity_base64url_decode(const char *in, uint8_t *out,
                                          size_t out_cap, size_t *out_len);

#ifdef MICHI_IDENTITY_TESTING
/**
 * @brief Test-only: reset the in-RAM component state to UNINITIALIZED
 *        WITHOUT touching NVS (simulates a reboot for host tests).
 *        Never compiled into the firmware build.
 */
void michi_identity_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
