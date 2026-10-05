#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "michi_auth.h"
#include "michi_home.h"
#include "michi_identity.h"

#define TAG "michi_auth"

#define MAX_ACTIVE_CHALLENGES 16
#define MAX_ACTIVE_SESSIONS 8

typedef struct {
    bool active;
    char challenge_id[MICHI_AUTH_CHALLENGE_ID_LEN];
    char nonce[MICHI_AUTH_NONCE_B64_LEN];
    char client_michi_id[MICHI_HOME_ID_LEN];
    char client_pk[MICHI_HOME_PUBKEY_B64_LEN];
    char home_id[MICHI_HOME_ID_LEN];
    int64_t expires_mono_us;
} auth_challenge_entry_t;

typedef struct {
    bool active;
    char session_token[MICHI_AUTH_TOKEN_B64_LEN];
    char client_michi_id[MICHI_HOME_ID_LEN];
    uint32_t permissions;
    int64_t expires_mono_us;
} auth_session_entry_t;

static auth_challenge_entry_t s_challenges[MAX_ACTIVE_CHALLENGES];
static auth_session_entry_t s_sessions[MAX_ACTIVE_SESSIONS];
static SemaphoreHandle_t s_mutex = NULL;

static void fill_random_bytes(uint8_t *buf, size_t len)
{
#ifndef MICHI_HOST_TEST
    esp_fill_random(buf, len);
#else
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(rand() & 0xFF);
    }
#endif
}

static void mint_uuid_v4(char *out, size_t out_len)
{
    if (out == NULL || out_len < 37) return;
    uint8_t b[16];
    fill_random_bytes(b, sizeof(b));
    b[6] = (b[6] & 0x0F) | 0x40; /* version 4 */
    b[8] = (b[8] & 0x3F) | 0x80; /* variant 10 */
    snprintf(out, out_len,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

esp_err_t michi_auth_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    michi_auth_reset();
    return ESP_OK;
}

void michi_auth_reset(void)
{
    if (s_mutex != NULL) {
        (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
    memset(s_challenges, 0, sizeof(s_challenges));
    memset(s_sessions, 0, sizeof(s_sessions));
    if (s_mutex != NULL) {
        (void)xSemaphoreGive(s_mutex);
    }
}

static void purge_expired_locked(int64_t now_us)
{
    for (size_t i = 0; i < MAX_ACTIVE_CHALLENGES; i++) {
        if (s_challenges[i].active && now_us >= s_challenges[i].expires_mono_us) {
            s_challenges[i].active = false;
        }
    }
    for (size_t i = 0; i < MAX_ACTIVE_SESSIONS; i++) {
        if (s_sessions[i].active && now_us >= s_sessions[i].expires_mono_us) {
            s_sessions[i].active = false;
        }
    }
}

esp_err_t michi_auth_create_challenge(
    const char *client_michi_id,
    const char *client_pk,
    const char *home_id,
    char *out_challenge_id,
    size_t challenge_id_len,
    char *out_nonce,
    size_t nonce_len,
    uint32_t *out_expires_in)
{
    if (client_michi_id == NULL || client_pk == NULL || home_id == NULL ||
        out_challenge_id == NULL || out_nonce == NULL || out_expires_in == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* If receiver has a configured home_id, home_id must match */
    if (michi_home_is_provisioned()) {
        char configured_home[MICHI_HOME_ID_LEN];
        if (michi_home_get_id(configured_home, sizeof(configured_home)) == ESP_OK) {
            if (strcmp(configured_home, home_id) != 0) {
                return ESP_ERR_NOT_ALLOWED; /* 403 Forbidden */
            }
        }
    }

    /* Verify that client_michi_id derives from client_pk */
    uint8_t pk_raw[32];
    size_t pk_raw_len = 0;
    if (michi_identity_base64url_decode(client_pk, pk_raw, sizeof(pk_raw), &pk_raw_len) != ESP_OK ||
        pk_raw_len != 32) {
        return ESP_ERR_INVALID_ARG;
    }
    char derived_id[MICHI_HOME_ID_LEN];
    if (michi_identity_derive_michi_id(pk_raw, derived_id, sizeof(derived_id)) != ESP_OK ||
        strcmp(derived_id, client_michi_id) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mutex == NULL) {
        michi_auth_init();
    }
    (void)xSemaphoreTake(s_mutex, portMAX_DELAY);

    const int64_t now_us = esp_timer_get_time();
    purge_expired_locked(now_us);

    /* DoS-safe slot allocation:
     * 1. If this client already has an active, non-expired challenge, return it (idempotency)
     * 2. Otherwise allocate the first available inactive slot
     * 3. If table is full of other clients' in-flight challenges, reject with ESP_ERR_NO_MEM
     *    (never evict other clients' active challenges) */
    int slot = -1;
    for (size_t i = 0; i < MAX_ACTIVE_CHALLENGES; i++) {
        if (s_challenges[i].active &&
            strcmp(s_challenges[i].client_michi_id, client_michi_id) == 0) {
            slot = (int)i;
            break;
        }
    }
    if (slot != -1) {
        int64_t remaining_us = s_challenges[slot].expires_mono_us - now_us;
        if (remaining_us > 0) {
            snprintf(out_challenge_id, challenge_id_len, "%s", s_challenges[slot].challenge_id);
            snprintf(out_nonce, nonce_len, "%s", s_challenges[slot].nonce);
            uint32_t rem_sec = (uint32_t)(remaining_us / 1000000LL);
            *out_expires_in = rem_sec > 0 ? rem_sec : 1;
            (void)xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    if (slot == -1) {
        for (size_t i = 0; i < MAX_ACTIVE_CHALLENGES; i++) {
            if (!s_challenges[i].active) {
                slot = (int)i;
                break;
            }
        }
    }
    if (slot == -1) {
        (void)xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    char cid[MICHI_AUTH_CHALLENGE_ID_LEN];
    mint_uuid_v4(cid, sizeof(cid));

    uint8_t nonce_raw[16];
    fill_random_bytes(nonce_raw, sizeof(nonce_raw));
    char nonce_b64[MICHI_AUTH_NONCE_B64_LEN];
    if (michi_identity_base64url_encode(nonce_raw, sizeof(nonce_raw), nonce_b64, sizeof(nonce_b64)) != ESP_OK) {
        (void)xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    s_challenges[slot].active = true;
    snprintf(s_challenges[slot].challenge_id, sizeof(s_challenges[slot].challenge_id), "%s", cid);
    snprintf(s_challenges[slot].nonce, sizeof(s_challenges[slot].nonce), "%s", nonce_b64);
    snprintf(s_challenges[slot].client_michi_id, sizeof(s_challenges[slot].client_michi_id), "%s", client_michi_id);
    snprintf(s_challenges[slot].client_pk, sizeof(s_challenges[slot].client_pk), "%s", client_pk);
    snprintf(s_challenges[slot].home_id, sizeof(s_challenges[slot].home_id), "%s", home_id);
    s_challenges[slot].expires_mono_us = now_us + (int64_t)MICHI_AUTH_CHALLENGE_TTL_SEC * 1000000LL;

    snprintf(out_challenge_id, challenge_id_len, "%s", cid);
    snprintf(out_nonce, nonce_len, "%s", nonce_b64);
    *out_expires_in = MICHI_AUTH_CHALLENGE_TTL_SEC;

    (void)xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t michi_auth_verify_and_create_session(
    const char *challenge_id,
    const char *client_michi_id,
    const michi_membership_t *membership,
    const char *client_signature_b64,
    char *out_session_token,
    size_t session_token_len,
    char *out_server_signature_b64,
    size_t server_signature_len,
    uint32_t *out_expires_in)
{
    if (challenge_id == NULL || client_michi_id == NULL || membership == NULL ||
        client_signature_b64 == NULL || out_session_token == NULL ||
        out_server_signature_b64 == NULL || out_expires_in == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mutex == NULL) {
        michi_auth_init();
    }
    (void)xSemaphoreTake(s_mutex, portMAX_DELAY);

    const int64_t now_us = esp_timer_get_time();
    purge_expired_locked(now_us);

    int challenge_slot = -1;
    for (size_t i = 0; i < MAX_ACTIVE_CHALLENGES; i++) {
        if (s_challenges[i].active &&
            strcmp(s_challenges[i].challenge_id, challenge_id) == 0) {
            challenge_slot = (int)i;
            break;
        }
    }

    if (challenge_slot == -1) {
        (void)xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND; /* 404 Not Found (or expired) */
    }

    auth_challenge_entry_t ch = s_challenges[challenge_slot];
    /* Do NOT deactivate challenge yet: verify membership, revocation, and signature first */
    (void)xSemaphoreGive(s_mutex);

    /* Check client_michi_id matches challenge */
    if (strcmp(ch.client_michi_id, client_michi_id) != 0 ||
        strcmp(ch.client_michi_id, membership->device_michi_id) != 0 ||
        strcmp(ch.home_id, membership->home_id) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Validate membership against home root public key */
    uint8_t root_pk[MICHI_HOME_KEY_BYTES];
    if (michi_home_get_root_public_key(root_pk) != ESP_OK) {
        /* If no home root is configured in NVS, cannot verify */
        return ESP_ERR_INVALID_STATE;
    }

    if (!michi_home_verify_membership(membership, root_pk)) {
        ESP_LOGW(TAG, "Membership verification FAILED for client %s", client_michi_id);
        return ESP_ERR_INVALID_RESPONSE; /* 401 Unauthorized */
    }

    if (michi_home_is_device_revoked(client_michi_id)) {
        ESP_LOGW(TAG, "Device %s is revoked in this home", client_michi_id);
        return ESP_ERR_INVALID_RESPONSE; /* 401 Unauthorized */
    }

    /* Enforce roles & compute permissions for receiver control */
    uint32_t perms = 0;
    for (size_t r = 0; r < membership->role_count; r++) {
        const char *role = membership->roles[r];
        if (strcmp(role, "music_server") == 0 ||
            strcmp(role, "remote_controller") == 0) {
            perms |= (MICHI_PERM_STATUS | MICHI_PERM_PLAYBACK | MICHI_PERM_VOLUME | MICHI_PERM_SETTINGS);
        } else if (strcmp(role, "playback_host") == 0 ||
                   strcmp(role, "desktop_player") == 0 ||
                   strcmp(role, "mobile_player") == 0) {
            perms |= (MICHI_PERM_STATUS | MICHI_PERM_PLAYBACK | MICHI_PERM_VOLUME);
        } else if (strcmp(role, "sync_host") == 0) {
            perms |= (MICHI_PERM_STATUS | MICHI_PERM_PLAYBACK);
        } else if (strcmp(role, "library_master") == 0) {
            perms |= (MICHI_PERM_STATUS | MICHI_PERM_SETTINGS | MICHI_PERM_OTA);
        } else if (strcmp(role, "sync_client") == 0 ||
                   strcmp(role, "library_host") == 0 ||
                   strcmp(role, "audio_receiver") == 0) {
            perms |= MICHI_PERM_STATUS;
        }
    }
    if (perms == 0) {
        ESP_LOGW(TAG, "Membership for %s has no authorized roles for receiver control", client_michi_id);
        return ESP_ERR_INVALID_ARG;
    }

    /* Validate client signature over device auth domain */
    uint8_t client_pk[MICHI_HOME_KEY_BYTES];
    size_t client_pk_len = 0;
    if (michi_identity_base64url_decode(membership->device_public_key, client_pk, sizeof(client_pk), &client_pk_len) != ESP_OK ||
        client_pk_len != 32) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t client_sig[MICHI_HOME_SIG_BYTES];
    size_t sig_len = 0;
    if (michi_identity_base64url_decode(client_signature_b64, client_sig, sizeof(client_sig), &sig_len) != ESP_OK ||
        sig_len != 64) {
        return ESP_ERR_INVALID_ARG;
    }

    char server_michi_id[MICHI_HOME_ID_LEN];
    if (michi_identity_michi_id(server_michi_id, sizeof(server_michi_id)) != ESP_OK) {
        return ESP_FAIL;
    }

    if (!michi_home_verify_device_auth(ch.home_id, server_michi_id, client_michi_id,
                                       ch.challenge_id, ch.nonce, client_sig, client_pk)) {
        ESP_LOGW(TAG, "Device auth challenge signature verification FAILED for %s", client_michi_id);
        return ESP_ERR_INVALID_RESPONSE; /* 401 Unauthorized */
    }

    /* Mint ephemeral session token (32 CSPRNG bytes -> 43 base64url chars) */
    uint8_t token_raw[32];
    fill_random_bytes(token_raw, sizeof(token_raw));
    char token_b64[MICHI_AUTH_TOKEN_B64_LEN];
    if (michi_identity_base64url_encode(token_raw, sizeof(token_raw), token_b64, sizeof(token_b64)) != ESP_OK) {
        return ESP_FAIL;
    }

    /* Create server mutual authentication confirmation signature */
    uint8_t server_sig[MICHI_HOME_SIG_BYTES];
    if (michi_home_sign_server_auth(ch.home_id, server_michi_id, client_michi_id,
                                   ch.challenge_id, token_b64, server_sig) != ESP_OK) {
        return ESP_FAIL;
    }
    char server_sig_b64[MICHI_HOME_SIG_B64_LEN];
    if (michi_identity_base64url_encode(server_sig, sizeof(server_sig), server_sig_b64, sizeof(server_sig_b64)) != ESP_OK) {
        return ESP_FAIL;
    }

    /* All verifications passed. Re-acquire lock to consume challenge atomically and allocate session */
    (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
    const int64_t session_now_us = esp_timer_get_time();
    purge_expired_locked(session_now_us);

    /* Verify challenge is still active in slot */
    if (!s_challenges[challenge_slot].active ||
        strcmp(s_challenges[challenge_slot].challenge_id, challenge_id) != 0) {
        (void)xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    s_challenges[challenge_slot].active = false;

    /* Session table:
     * 1. If this client already has an active session, rotate it (reuse slot)
     * 2. Otherwise find a free slot
     * 3. If full: return ESP_ERR_NO_MEM (HTTP 429), NEVER arbitrarily evict slot 0 */
    int session_slot = -1;
    for (size_t i = 0; i < MAX_ACTIVE_SESSIONS; i++) {
        if (s_sessions[i].active &&
            strcmp(s_sessions[i].client_michi_id, client_michi_id) == 0) {
            session_slot = (int)i;
            break;
        }
    }
    if (session_slot == -1) {
        for (size_t i = 0; i < MAX_ACTIVE_SESSIONS; i++) {
            if (!s_sessions[i].active) {
                session_slot = (int)i;
                break;
            }
        }
    }
    if (session_slot == -1) {
        (void)xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    s_sessions[session_slot].active = true;
    snprintf(s_sessions[session_slot].session_token, sizeof(s_sessions[session_slot].session_token), "%s", token_b64);
    snprintf(s_sessions[session_slot].client_michi_id, sizeof(s_sessions[session_slot].client_michi_id), "%s", client_michi_id);
    s_sessions[session_slot].permissions = perms;
    s_sessions[session_slot].expires_mono_us = session_now_us + (int64_t)MICHI_AUTH_SESSION_TTL_SEC * 1000000LL;

    (void)xSemaphoreGive(s_mutex);

    snprintf(out_session_token, session_token_len, "%s", token_b64);
    snprintf(out_server_signature_b64, server_signature_len, "%s", server_sig_b64);
    *out_expires_in = MICHI_AUTH_SESSION_TTL_SEC;

    ESP_LOGI(TAG, "Device session ESTABLISHED for client %s (perms=0x%" PRIx32 ")", client_michi_id, perms);
    return ESP_OK;
}

esp_err_t michi_auth_validate_token_perm(
    const char *token,
    uint32_t perm,
    char *out_client_michi_id,
    size_t client_michi_id_len)
{
    if (token == NULL || s_mutex == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
    const int64_t now_us = esp_timer_get_time();
    purge_expired_locked(now_us);

    for (size_t i = 0; i < MAX_ACTIVE_SESSIONS; i++) {
        if (s_sessions[i].active && strcmp(s_sessions[i].session_token, token) == 0) {
            if (perm != 0 && (s_sessions[i].permissions & perm) != perm) {
                (void)xSemaphoreGive(s_mutex);
                return ESP_ERR_INVALID_STATE; /* 403 Forbidden: lacks required permission */
            }
            if (out_client_michi_id != NULL && client_michi_id_len > 0) {
                snprintf(out_client_michi_id, client_michi_id_len, "%s", s_sessions[i].client_michi_id);
            }
            /* Refresh session TTL on active use */
            s_sessions[i].expires_mono_us = now_us + (int64_t)MICHI_AUTH_SESSION_TTL_SEC * 1000000LL;
            (void)xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }

    (void)xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND; /* 401 Unauthorized: token not found or expired */
}

bool michi_auth_validate_token(
    const char *token,
    char *out_client_michi_id,
    size_t client_michi_id_len)
{
    return michi_auth_validate_token_perm(token, 0, out_client_michi_id, client_michi_id_len) == ESP_OK;
}
