#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "michi_home.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MICHI_AUTH_CHALLENGE_ID_LEN 37    /* UUID v4 string + NUL */
#define MICHI_AUTH_NONCE_B64_LEN 23       /* 16 bytes base64url-nopad (22 chars + NUL) */
#define MICHI_AUTH_TOKEN_B64_LEN 44       /* 32 bytes base64url-nopad (43 chars + NUL) */

#define MICHI_AUTH_CHALLENGE_TTL_SEC 60
#define MICHI_AUTH_SESSION_TTL_SEC 3600

esp_err_t michi_auth_init(void);

/* Create a new challenge in RAM */
esp_err_t michi_auth_create_challenge(
    const char *client_michi_id,
    const char *client_pk,
    const char *home_id,
    char *out_challenge_id,
    size_t challenge_id_len,
    char *out_nonce,
    size_t nonce_len,
    uint32_t *out_expires_in);

/* Verify challenge answer, membership, and create session in RAM */
esp_err_t michi_auth_verify_and_create_session(
    const char *challenge_id,
    const char *client_michi_id,
    const michi_membership_t *membership,
    const char *client_signature_b64,
    char *out_session_token,
    size_t session_token_len,
    char *out_server_signature_b64,
    size_t server_signature_len,
    uint32_t *out_expires_in);

#define MICHI_PERM_STATUS        0x00000001u /*!< Read status/state/diagnostics */
#define MICHI_PERM_PLAYBACK      0x00000002u /*!< Start/stop/pause playback */
#define MICHI_PERM_VOLUME        0x00000004u /*!< Read/set volume */
#define MICHI_PERM_SETTINGS      0x00000008u /*!< Read/change device settings */
#define MICHI_PERM_OTA           0x00000020u /*!< Trigger/authorize OTA */
#define MICHI_PERM_DEFAULT       (MICHI_PERM_STATUS | MICHI_PERM_PLAYBACK | MICHI_PERM_VOLUME | MICHI_PERM_SETTINGS)

/* Validate a Bearer session token against active RAM sessions and check permissions */
esp_err_t michi_auth_validate_token_perm(
    const char *token,
    uint32_t perm,
    char *out_client_michi_id,
    size_t client_michi_id_len);

/* Validate a Bearer session token against active RAM sessions (any permission) */
bool michi_auth_validate_token(
    const char *token,
    char *out_client_michi_id,
    size_t client_michi_id_len);

/* Reset in-RAM challenges and sessions */
void michi_auth_reset(void);

#ifdef __cplusplus
}
#endif
