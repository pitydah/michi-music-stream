#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MICHI_HOME_ID_LEN 44          /* 43 chars base64url + NUL */
#define MICHI_HOME_KEY_BYTES 32
#define MICHI_HOME_PUBKEY_B64_LEN 44
#define MICHI_HOME_SIG_BYTES 64
#define MICHI_HOME_SIG_B64_LEN 87

#define MICHI_HOME_NVS_NAMESPACE "michi_home"
#define MICHI_HOME_NVS_KEY_ID "home_id"
#define MICHI_HOME_NVS_KEY_ROOT_PK "root_pk"

/* Canonical domain separation prefixes */
#define MICHI_HOME_DOMAIN_DEVICE_AUTH "michi-link-device-auth-v1"
#define MICHI_HOME_DOMAIN_SERVER_AUTH "michi-link-server-auth-v1"
#define MICHI_HOME_DOMAIN_MEMBERSHIP  "michi-link-membership-v1"
#define MICHI_HOME_DOMAIN_REVOCATION  "michi-link-revocation-v1"

#define MICHI_MAX_MEMBERSHIP_ROLES 16
#define MICHI_MAX_ROLE_NAME_LEN 32

typedef struct {
    uint32_t version;
    char home_id[MICHI_HOME_ID_LEN];
    char device_michi_id[MICHI_HOME_ID_LEN];
    char device_public_key[MICHI_HOME_PUBKEY_B64_LEN];
    char device_type[32];
    char roles[MICHI_MAX_MEMBERSHIP_ROLES][MICHI_MAX_ROLE_NAME_LEN];
    size_t role_count;
    char issued_at[40];
    uint64_t serial;
    char signature[MICHI_HOME_SIG_B64_LEN];
} michi_membership_t;

esp_err_t michi_home_init(void);
bool michi_home_is_provisioned(void);
esp_err_t michi_home_get_id(char *out, size_t out_len);
esp_err_t michi_home_get_root_public_key(uint8_t out[MICHI_HOME_KEY_BYTES]);
esp_err_t michi_home_get_root_public_key_b64(char *out, size_t out_len);
esp_err_t michi_home_set_credentials(const char *home_id, const uint8_t root_pk[MICHI_HOME_KEY_BYTES]);
esp_err_t michi_home_erase(void);

/* Build canonical membership payload bytes for signing / verification */
size_t michi_home_canonical_membership_bytes(
    const char *home_id,
    const char *device_michi_id,
    const char *device_public_key,
    const char *device_type,
    const char roles[MICHI_MAX_MEMBERSHIP_ROLES][MICHI_MAX_ROLE_NAME_LEN],
    size_t role_count,
    const char *issued_at,
    uint64_t serial,
    uint8_t *out,
    size_t out_len);

/* Verify membership certificate */
bool michi_home_verify_membership(
    const michi_membership_t *membership,
    const uint8_t root_pk[MICHI_HOME_KEY_BYTES]);

/* Build device auth challenge payload */
size_t michi_home_device_auth_payload(
    const char *home_id,
    const char *server_michi_id,
    const char *client_michi_id,
    const char *challenge_id,
    const char *challenge_nonce,
    uint8_t *out,
    size_t out_len);

/* Verify device auth challenge signature */
bool michi_home_verify_device_auth(
    const char *home_id,
    const char *server_michi_id,
    const char *client_michi_id,
    const char *challenge_id,
    const char *challenge_nonce,
    const uint8_t sig[MICHI_HOME_SIG_BYTES],
    const uint8_t client_pk[MICHI_HOME_KEY_BYTES]);

/* Build & sign server auth session payload */
esp_err_t michi_home_sign_server_auth(
    const char *home_id,
    const char *server_michi_id,
    const char *client_michi_id,
    const char *challenge_id,
    const char *session_token,
    uint8_t out_sig[MICHI_HOME_SIG_BYTES]);

#define MICHI_HOME_NVS_KEY_MEMBERSHIP "membership"

/* Receiver device membership */
esp_err_t michi_home_set_device_membership(const michi_membership_t *membership);
esp_err_t michi_home_get_device_membership(michi_membership_t *out_membership);
bool michi_home_has_device_membership(void);

#define MICHI_MAX_REVOCATIONS 16

typedef struct {
    uint32_t version;
    char home_id[MICHI_HOME_ID_LEN];
    char revoked_device_michi_id[MICHI_HOME_ID_LEN];
    char revoked_at[40];
    char reason[256];
    char signature[MICHI_HOME_SIG_B64_LEN];
} michi_revocation_t;

/* Canonical revocation payload bytes */
size_t michi_home_canonical_revocation_bytes(
    const char *home_id,
    const char *revoked_device_michi_id,
    const char *revoked_at,
    const char *reason,
    uint8_t *out,
    size_t out_len);

/* Verify revocation certificate */
bool michi_home_verify_revocation(
    const michi_revocation_t *revocation,
    const uint8_t root_pk[MICHI_HOME_KEY_BYTES]);

/* Revocation list management */
typedef void (*michi_home_revocation_cb_t)(const char *revoked_device_michi_id);
void michi_home_set_revocation_callback(michi_home_revocation_cb_t cb);
size_t michi_home_get_revocations(michi_revocation_t *out_revocations, size_t max_count);
esp_err_t michi_home_add_revocation(const michi_revocation_t *revocation);
bool michi_home_is_device_revoked(const char *device_michi_id);

/* Factory configuration MICHI-F1 */
#define MICHI_F1_MAGIC "MICHI-F1"
#define MICHI_F1_MAGIC_LEN 8
#define MICHI_F1_VERSION 1
#define MICHI_F1_NONCE_MIN_LEN 16
#define MICHI_F1_NONCE_MAX_LEN 32

/* Canonical Ecosystem wire format (27 bytes header: magic[8] | schema(u8) | nonce[16] | payload_len(u16be)) */
typedef struct __attribute__((packed)) {
    char magic[8];        /* "MICHI-F1" */
    uint8_t schema;       /* 1 */
    uint8_t nonce[16];    /* 16 bytes */
    uint16_t payload_len; /* Big-endian */
} michi_f1_ecosystem_header_t;

typedef struct __attribute__((packed)) {
    char magic[8];            /* "MICHI-F1" */
    uint16_t version;         /* 1 */
    uint16_t nonce_len;       /* 16..32 */
    uint8_t nonce[32];
    uint32_t payload_len;
} michi_f1_header_t;

typedef struct {
    char home_id[MICHI_HOME_ID_LEN];
    char root_public_key[MICHI_HOME_PUBKEY_B64_LEN];
    char wifi_ssid[64];
    char wifi_password[64];
    bool has_device_seed;
    uint8_t device_seed[MICHI_HOME_KEY_BYTES];
    bool has_device_membership;
    michi_membership_t device_membership;
} michi_factory_cfg_t;

esp_err_t michi_factory_cfg_parse(const char *payload, size_t len, michi_factory_cfg_t *out_cfg);
esp_err_t michi_home_import_factory_cfg(const char *payload, size_t len);
esp_err_t michi_factory_cfg_check_and_import(void);

#ifdef MICHI_HOME_TESTING
void michi_home_test_reset(void);
void michi_factory_cfg_set_test_partition_data(const char *data);
void michi_factory_cfg_set_test_partition_bytes(const uint8_t *data, size_t len);
#endif

#ifdef __cplusplus
}
#endif
