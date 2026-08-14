/* Persistent Michi identity (MS-04): Ed25519 RFC 8032 + BLAKE3, scheme
 * "ed25519-blake3-v1", byte-compatible with the Michi Link michi-identity
 * crate.
 *
 * Cryptographic flow (contract v1, firmware):
 *
 *   esp_fill_random (hardware RNG)
 *         |
 *         v
 *      seed[32] --persist ONCE in NVS--> "michi_identity"/"seed"
 *         |
 *         +--> crypto_ed25519_key_pair --> public_key[32]
 *                                            |
 *                                            +--> blake3 --> michi_id
 *                                                          (base64url, 43c)
 *
 * Wire values are base64url WITHOUT padding (public key 43 chars,
 * signature 86 chars, michi_id 43 chars). Signatures are Ed25519
 * RFC 8032 (SHA-512), interoperable with ed25519-dalek.
 *
 * Corruption contract: the store is never silently regenerated. A
 * corrupt store (read error, wrong length, wrong version) puts the
 * component in MICHI_IDENTITY_CORRUPT; only michi_identity_factory_reset()
 * (an explicit user-initiated action) clears it, and a fresh identity is
 * only minted by the subsequent explicit init().
 *
 * Zero secrets in logs: the seed/secret key are never logged; only the
 * public michi_id appears.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"

#include "blake3.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"

#include "identity_storage.h"
#include "michi_identity.h"

#define TAG "michi_identity"

/* ESP-IDF's nvs uses ESP_ERR_NVS_NOT_FOUND (a distinct code from
 * ESP_ERR_NOT_FOUND) for missing namespaces/keys. Both mean "no store
 * yet" here. */
#define IDENTITY_NOT_FOUND(err) \
    ((err) == ESP_ERR_NOT_FOUND || (err) == ESP_ERR_NVS_NOT_FOUND)

static const char BASE64URL_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* In-RAM identity. s_secret holds the expanded Ed25519 secret key
 * (seed || public key, 64 bytes); s_state owns every transition. */
static michi_identity_state_t s_state = MICHI_IDENTITY_UNINITIALIZED;
static uint8_t s_secret[MICHI_IDENTITY_SIGNATURE_BYTES];
static uint8_t s_public_key[MICHI_IDENTITY_KEY_BYTES];
static char s_michi_id[MICHI_IDENTITY_MICHI_ID_LEN];

/* --- base64url (no padding) ------------------------------------------- */

esp_err_t michi_identity_base64url_encode(const uint8_t *in, size_t in_len,
                                          char *out, size_t out_len)
{
    if (in == NULL || out == NULL || in_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 4 chars per full group of 3, rem+1 chars for the tail group, + NUL. */
    size_t required = (in_len / 3) * 4 + 1;
    if (in_len % 3 != 0) {
        required += in_len % 3 + 1;
    }
    if (out_len < required) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t o = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        uint32_t n = (uint32_t)in[i] << 16;
        size_t remain = in_len - i;
        if (remain > 1) {
            n |= (uint32_t)in[i + 1] << 8;
        }
        if (remain > 2) {
            n |= (uint32_t)in[i + 2];
        }
        out[o++] = BASE64URL_ALPHABET[(n >> 18) & 0x3F];
        out[o++] = BASE64URL_ALPHABET[(n >> 12) & 0x3F];
        if (remain > 1) {
            out[o++] = BASE64URL_ALPHABET[(n >> 6) & 0x3F];
        }
        if (remain > 2) {
            out[o++] = BASE64URL_ALPHABET[n & 0x3F];
        }
    }
    out[o] = '\0';
    return ESP_OK;
}

static int base64url_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '-') {
        return 62;
    }
    if (c == '_') {
        return 63;
    }
    return -1; /* includes '=', '+' and '/' (strict) */
}

esp_err_t michi_identity_base64url_decode(const char *in, uint8_t *out,
                                          size_t out_cap, size_t *out_len)
{
    if (in == NULL || out == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t acc = 0;
    uint32_t bits = 0;
    size_t o = 0;
    for (const char *p = in; *p != '\0'; p++) {
        int v = base64url_value(*p);
        if (v < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= out_cap) {
                return ESP_ERR_INVALID_SIZE;
            }
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    /* Reject dangling bits: a no-padding encoding of whole bytes always
     * ends on a byte boundary. */
    if (bits >= 6) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = o;
    return ESP_OK;
}

/* --- identity derivation ---------------------------------------------- */

esp_err_t michi_identity_derive_michi_id(const uint8_t pk[MICHI_IDENTITY_KEY_BYTES],
                                         char *out, size_t out_len)
{
    if (pk == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* michi_id = base64url-nopad(blake3(public_key_bytes)) - exactly the
     * MichiId::from_public_key derivation of the Rust crate. */
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, pk, MICHI_IDENTITY_KEY_BYTES);
    uint8_t hash[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&hasher, hash, sizeof(hash));

    return michi_identity_base64url_encode(hash, sizeof(hash), out, out_len);
}

/* --- crypto helpers ---------------------------------------------------- */

static void wipe_ram_identity(void)
{
    crypto_wipe(s_secret, sizeof(s_secret));
    crypto_wipe(s_public_key, sizeof(s_public_key));
    crypto_wipe(s_michi_id, sizeof(s_michi_id));
}

/* Expands the seed and fills pk + michi_id. Never touches NVS. */
static esp_err_t derive_from_seed(const uint8_t seed[MICHI_IDENTITY_SEED_BYTES])
{
    uint8_t seed_copy[MICHI_IDENTITY_SEED_BYTES];
    memcpy(seed_copy, seed, sizeof(seed_copy));
    /* crypto_ed25519_key_pair wipes its seed argument; the caller's copy
     * must survive for persistence. */
    crypto_ed25519_key_pair(s_secret, s_public_key, seed_copy);
    crypto_wipe(seed_copy, sizeof(seed_copy));

    esp_err_t err = michi_identity_derive_michi_id(s_public_key, s_michi_id,
                                                   sizeof(s_michi_id));
    if (err != ESP_OK) {
        wipe_ram_identity();
        return err;
    }
    return ESP_OK;
}

static esp_err_t generate_and_persist(void)
{
    uint8_t seed[MICHI_IDENTITY_SEED_BYTES];
    /* Hardware RNG - the ONLY entropy source for the seed. */
    esp_fill_random(seed, sizeof(seed));

    esp_err_t err = derive_from_seed(seed);
    if (err != ESP_OK) {
        crypto_wipe(seed, sizeof(seed));
        return err;
    }

    michi_identity_blob_t blob = {
        .version = MICHI_IDENTITY_BLOB_VERSION,
        .reserved = {0},
    };
    memcpy(blob.seed, seed, sizeof(blob.seed));
    crypto_wipe(seed, sizeof(seed));

    err = michi_identity_nvs_store(&blob);
    if (err != ESP_OK) {
        /* Nothing persisted: the boot may retry init(); do NOT enter
         * CORRUPT (the store is empty) and do NOT expose the keys. */
        wipe_ram_identity();
        return err;
    }
    return ESP_OK;
}

/* --- state machine ----------------------------------------------------- */

esp_err_t michi_identity_init(void)
{
    if (s_state == MICHI_IDENTITY_READY) {
        return ESP_OK;
    }
    if (s_state == MICHI_IDENTITY_CORRUPT) {
        ESP_LOGE(TAG, "identity: corrupt (factory reset required)");
        return ESP_ERR_INVALID_STATE;
    }

    michi_identity_blob_t blob;
    esp_err_t err = michi_identity_nvs_load(&blob);
    if (IDENTITY_NOT_FOUND(err)) {
        /* First boot: generate + persist exactly once. */
        err = generate_and_persist();
        if (err != ESP_OK) {
            return err;
        }
        s_state = MICHI_IDENTITY_READY;
        ESP_LOGI(TAG, "identity: generated michi_id=%s", s_michi_id);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        s_state = MICHI_IDENTITY_CORRUPT;
        ESP_LOGE(TAG, "identity: store_read_failed err=%s",
                 esp_err_to_name(err));
        return err;
    }
    if (blob.version != MICHI_IDENTITY_BLOB_VERSION) {
        s_state = MICHI_IDENTITY_CORRUPT;
        ESP_LOGE(TAG, "identity: unsupported_blob_version");
        return ESP_ERR_INVALID_SIZE;
    }

    err = derive_from_seed(blob.seed);
    if (err != ESP_OK) {
        s_state = MICHI_IDENTITY_CORRUPT;
        return err;
    }
    s_state = MICHI_IDENTITY_READY;
    ESP_LOGI(TAG, "identity: loaded michi_id=%s", s_michi_id);
    return ESP_OK;
}

michi_identity_state_t michi_identity_get_state(void)
{
    return s_state;
}

esp_err_t michi_identity_factory_reset(void)
{
    wipe_ram_identity();
    esp_err_t err = michi_identity_nvs_erase();
    if (err != ESP_OK) {
        return err;
    }
    s_state = MICHI_IDENTITY_UNINITIALIZED;
    ESP_LOGW(TAG, "identity: erased (explicit factory reset)");
    return ESP_OK;
}

/* --- key access -------------------------------------------------------- */

esp_err_t michi_identity_public_key(uint8_t out[MICHI_IDENTITY_KEY_BYTES])
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state != MICHI_IDENTITY_READY) {
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(out, s_public_key, MICHI_IDENTITY_KEY_BYTES);
    return ESP_OK;
}

esp_err_t michi_identity_michi_id(char *out, size_t out_len)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state != MICHI_IDENTITY_READY) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_len < MICHI_IDENTITY_MICHI_ID_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, s_michi_id, MICHI_IDENTITY_MICHI_ID_LEN);
    return ESP_OK;
}

/* --- sign / verify ----------------------------------------------------- */

esp_err_t michi_identity_sign(const uint8_t *msg, size_t msg_len,
                              uint8_t sig[MICHI_IDENTITY_SIGNATURE_BYTES])
{
    if (sig == NULL || (msg == NULL && msg_len != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state != MICHI_IDENTITY_READY) {
        return ESP_ERR_INVALID_STATE;
    }
    crypto_ed25519_sign(sig, s_secret, msg, msg_len);
    return ESP_OK;
}

bool michi_identity_verify(const uint8_t *msg, size_t msg_len,
                           const uint8_t sig[MICHI_IDENTITY_SIGNATURE_BYTES],
                           const uint8_t pk[MICHI_IDENTITY_KEY_BYTES])
{
    if (sig == NULL || pk == NULL || (msg == NULL && msg_len != 0)) {
        return false;
    }
    /* Strict Ed25519 verification (RFC 8032): off-curve points and
     * non-canonical S are rejected - matches ed25519-dalek's
     * verify_strict semantics used by the Rust implementation. */
    return crypto_ed25519_check(sig, pk, msg, msg_len) == 0;
}

#ifdef MICHI_IDENTITY_TESTING
void michi_identity_test_reset(void)
{
    wipe_ram_identity();
    s_state = MICHI_IDENTITY_UNINITIALIZED;
}
#endif
