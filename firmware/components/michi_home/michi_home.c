#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"

#include "michi_home.h"
#include "michi_identity.h"

#define TAG "michi_home"

static bool s_provisioned = false;
static char s_home_id[MICHI_HOME_ID_LEN] = {0};
static uint8_t s_root_pk[MICHI_HOME_KEY_BYTES] = {0};
static michi_revocation_t s_revocations[MICHI_MAX_REVOCATIONS];
static size_t s_revocation_count = 0;

esp_err_t michi_home_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MICHI_HOME_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        s_provisioned = false;
        return ESP_OK; /* Unprovisioned is normal on first boot */
    }

    size_t id_len = sizeof(s_home_id);
    err = nvs_get_str(h, MICHI_HOME_NVS_KEY_ID, s_home_id, &id_len);
    if (err != ESP_OK) {
        nvs_close(h);
        s_provisioned = false;
        return ESP_OK;
    }

    size_t pk_len = sizeof(s_root_pk);
    err = nvs_get_blob(h, MICHI_HOME_NVS_KEY_ROOT_PK, s_root_pk, &pk_len);
    nvs_close(h);

    if (err != ESP_OK || pk_len != sizeof(s_root_pk)) {
        s_provisioned = false;
        memset(s_home_id, 0, sizeof(s_home_id));
        memset(s_root_pk, 0, sizeof(s_root_pk));
        return ESP_OK;
    }

    s_provisioned = true;
    ESP_LOGI(TAG, "Michi Home initialized: home_id=%s", s_home_id);
    return ESP_OK;
}

bool michi_home_is_provisioned(void)
{
    return s_provisioned;
}

esp_err_t michi_home_get_id(char *out, size_t out_len)
{
    if (out == NULL || out_len < MICHI_HOME_ID_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_provisioned) {
        return ESP_ERR_NOT_FOUND;
    }
    snprintf(out, out_len, "%s", s_home_id);
    return ESP_OK;
}

esp_err_t michi_home_get_root_public_key(uint8_t out[MICHI_HOME_KEY_BYTES])
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_provisioned) {
        return ESP_ERR_NOT_FOUND;
    }
    memcpy(out, s_root_pk, MICHI_HOME_KEY_BYTES);
    return ESP_OK;
}

esp_err_t michi_home_get_root_public_key_b64(char *out, size_t out_len)
{
    if (out == NULL || out_len < MICHI_HOME_PUBKEY_B64_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_provisioned) {
        return ESP_ERR_NOT_FOUND;
    }
    return michi_identity_base64url_encode(s_root_pk, MICHI_HOME_KEY_BYTES, out, out_len);
}

esp_err_t michi_home_set_credentials(const char *home_id, const uint8_t root_pk[MICHI_HOME_KEY_BYTES])
{
    if (home_id == NULL || strlen(home_id) != 43 || root_pk == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(MICHI_HOME_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(h, MICHI_HOME_NVS_KEY_ID, home_id);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, MICHI_HOME_NVS_KEY_ROOT_PK, root_pk, MICHI_HOME_KEY_BYTES);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        snprintf(s_home_id, sizeof(s_home_id), "%s", home_id);
        memcpy(s_root_pk, root_pk, MICHI_HOME_KEY_BYTES);
        s_provisioned = true;
    }
    return err;
}

esp_err_t michi_home_set_device_membership(const michi_membership_t *membership)
{
    if (membership == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_provisioned) {
        if (!michi_home_verify_membership(membership, s_root_pk)) {
            ESP_LOGE(TAG, "device membership verification failed under home root authority");
            return ESP_ERR_INVALID_ARG;
        }
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(MICHI_HOME_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(h, MICHI_HOME_NVS_KEY_MEMBERSHIP, membership, sizeof(*membership));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Device membership saved to NVS (device_michi_id=%s)", membership->device_michi_id);
    }
    return err;
}

esp_err_t michi_home_get_device_membership(michi_membership_t *out_membership)
{
    if (out_membership == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(MICHI_HOME_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    size_t mem_len = sizeof(*out_membership);
    err = nvs_get_blob(h, MICHI_HOME_NVS_KEY_MEMBERSHIP, out_membership, &mem_len);
    nvs_close(h);

    if (err != ESP_OK || mem_len != sizeof(*out_membership)) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

bool michi_home_has_device_membership(void)
{
    michi_membership_t mem;
    return michi_home_get_device_membership(&mem) == ESP_OK;
}

esp_err_t michi_home_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MICHI_HOME_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        (void)nvs_erase_key(h, MICHI_HOME_NVS_KEY_ID);
        (void)nvs_erase_key(h, MICHI_HOME_NVS_KEY_ROOT_PK);
        (void)nvs_erase_key(h, MICHI_HOME_NVS_KEY_MEMBERSHIP);
        (void)nvs_commit(h);
        nvs_close(h);
    }
    s_provisioned = false;
    memset(s_home_id, 0, sizeof(s_home_id));
    memset(s_root_pk, 0, sizeof(s_root_pk));
    s_revocation_count = 0;
    memset(s_revocations, 0, sizeof(s_revocations));
    return ESP_OK;
}

#ifdef MICHI_HOME_TESTING
void michi_home_test_reset(void)
{
    s_provisioned = false;
    memset(s_home_id, 0, sizeof(s_home_id));
    memset(s_root_pk, 0, sizeof(s_root_pk));
    s_revocation_count = 0;
    memset(s_revocations, 0, sizeof(s_revocations));
}
#endif

static int compare_roles(const void *a, const void *b)
{
    const char *str_a = (const char *)a;
    const char *str_b = (const char *)b;
    return strcmp(str_a, str_b);
}

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
    size_t out_len)
{
    if (home_id == NULL || device_michi_id == NULL || device_public_key == NULL ||
        device_type == NULL || issued_at == NULL || out == NULL || out_len == 0) {
        return 0;
    }

    /* Copy and sort roles lexicographically */
    char sorted_roles[MICHI_MAX_MEMBERSHIP_ROLES][MICHI_MAX_ROLE_NAME_LEN];
    if (role_count > MICHI_MAX_MEMBERSHIP_ROLES) {
        role_count = MICHI_MAX_MEMBERSHIP_ROLES;
    }
    for (size_t i = 0; i < role_count; i++) {
        snprintf(sorted_roles[i], sizeof(sorted_roles[i]), "%s", roles[i]);
    }
    if (role_count > 1) {
        qsort(sorted_roles, role_count, sizeof(sorted_roles[0]), compare_roles);
    }

    size_t written = 0;
    #define APPEND(data, len) do { \
        if (written + (len) > out_len) return 0; \
        memcpy(out + written, (data), (len)); \
        written += (len); \
    } while (0)

    APPEND(MICHI_HOME_DOMAIN_MEMBERSHIP, strlen(MICHI_HOME_DOMAIN_MEMBERSHIP));
    APPEND(home_id, strlen(home_id));
    APPEND(device_michi_id, strlen(device_michi_id));
    APPEND(device_public_key, strlen(device_public_key));
    APPEND(device_type, strlen(device_type));

    for (size_t i = 0; i < role_count; i++) {
        APPEND(":", 1);
        APPEND(sorted_roles[i], strlen(sorted_roles[i]));
    }

    APPEND(":", 1);
    APPEND(issued_at, strlen(issued_at));
    APPEND(":", 1);

    char serial_str[32];
    int n = snprintf(serial_str, sizeof(serial_str), "%" PRIu64, serial);
    if (n <= 0) return 0;
    APPEND(serial_str, (size_t)n);

    #undef APPEND
    return written;
}

bool michi_home_verify_membership(
    const michi_membership_t *membership,
    const uint8_t root_pk[MICHI_HOME_KEY_BYTES])
{
    if (membership == NULL || root_pk == NULL) {
        return false;
    }
    if (membership->version != 1) {
        return false;
    }

    /* Coherence check: device_michi_id must derive from device_public_key */
    uint8_t dev_pk[MICHI_HOME_KEY_BYTES];
    size_t dev_pk_len = 0;
    if (michi_identity_base64url_decode(membership->device_public_key, dev_pk, sizeof(dev_pk), &dev_pk_len) != ESP_OK ||
        dev_pk_len != MICHI_HOME_KEY_BYTES) {
        return false;
    }

    char derived_id[MICHI_HOME_ID_LEN];
    if (michi_identity_derive_michi_id(dev_pk, derived_id, sizeof(derived_id)) != ESP_OK) {
        return false;
    }
    if (strcmp(derived_id, membership->device_michi_id) != 0) {
        return false;
    }

    /* Check home_id matches if local home is configured */
    if (s_provisioned && strcmp(s_home_id, membership->home_id) != 0) {
        return false;
    }

    /* Decode membership signature */
    uint8_t sig[MICHI_HOME_SIG_BYTES];
    size_t sig_len = 0;
    if (michi_identity_base64url_decode(membership->signature, sig, sizeof(sig), &sig_len) != ESP_OK ||
        sig_len != MICHI_HOME_SIG_BYTES) {
        return false;
    }

    uint8_t canonical_buf[512];
    size_t canon_len = michi_home_canonical_membership_bytes(
        membership->home_id,
        membership->device_michi_id,
        membership->device_public_key,
        membership->device_type,
        membership->roles,
        membership->role_count,
        membership->issued_at,
        membership->serial,
        canonical_buf,
        sizeof(canonical_buf));

    if (canon_len == 0) {
        return false;
    }

    return michi_identity_verify(canonical_buf, canon_len, sig, root_pk);
}

size_t michi_home_device_auth_payload(
    const char *home_id,
    const char *server_michi_id,
    const char *client_michi_id,
    const char *challenge_id,
    const char *challenge_nonce,
    uint8_t *out,
    size_t out_len)
{
    if (home_id == NULL || server_michi_id == NULL || client_michi_id == NULL ||
        challenge_id == NULL || challenge_nonce == NULL || out == NULL) {
        return 0;
    }

    size_t written = 0;
    #define APPEND(data, len) do { \
        if (written + (len) > out_len) return 0; \
        memcpy(out + written, (data), (len)); \
        written += (len); \
    } while (0)

    APPEND(MICHI_HOME_DOMAIN_DEVICE_AUTH, strlen(MICHI_HOME_DOMAIN_DEVICE_AUTH));
    APPEND(home_id, strlen(home_id));
    APPEND(server_michi_id, strlen(server_michi_id));
    APPEND(client_michi_id, strlen(client_michi_id));
    APPEND(challenge_id, strlen(challenge_id));
    APPEND(challenge_nonce, strlen(challenge_nonce));

    #undef APPEND
    return written;
}

bool michi_home_verify_device_auth(
    const char *home_id,
    const char *server_michi_id,
    const char *client_michi_id,
    const char *challenge_id,
    const char *challenge_nonce,
    const uint8_t sig[MICHI_HOME_SIG_BYTES],
    const uint8_t client_pk[MICHI_HOME_KEY_BYTES])
{
    if (sig == NULL || client_pk == NULL) {
        return false;
    }

    uint8_t payload[256];
    size_t payload_len = michi_home_device_auth_payload(
        home_id, server_michi_id, client_michi_id, challenge_id, challenge_nonce,
        payload, sizeof(payload));

    if (payload_len == 0) {
        return false;
    }

    return michi_identity_verify(payload, payload_len, sig, client_pk);
}

esp_err_t michi_home_sign_server_auth(
    const char *home_id,
    const char *server_michi_id,
    const char *client_michi_id,
    const char *challenge_id,
    const char *session_token,
    uint8_t out_sig[MICHI_HOME_SIG_BYTES])
{
    if (home_id == NULL || server_michi_id == NULL || client_michi_id == NULL ||
        challenge_id == NULL || session_token == NULL || out_sig == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[256];
    size_t written = 0;
    #define APPEND(data, len) do { \
        if (written + (len) > sizeof(payload)) return ESP_ERR_INVALID_SIZE; \
        memcpy(payload + written, (data), (len)); \
        written += (len); \
    } while (0)

    APPEND(MICHI_HOME_DOMAIN_SERVER_AUTH, strlen(MICHI_HOME_DOMAIN_SERVER_AUTH));
    APPEND(home_id, strlen(home_id));
    APPEND(server_michi_id, strlen(server_michi_id));
    APPEND(client_michi_id, strlen(client_michi_id));
    APPEND(challenge_id, strlen(challenge_id));
    APPEND(session_token, strlen(session_token));

    #undef APPEND

    return michi_identity_sign(payload, written, out_sig);
}

size_t michi_home_canonical_revocation_bytes(
    const char *home_id,
    const char *revoked_device_michi_id,
    const char *revoked_at,
    const char *reason,
    uint8_t *out,
    size_t out_len)
{
    if (home_id == NULL || revoked_device_michi_id == NULL ||
        revoked_at == NULL || reason == NULL || out == NULL || out_len == 0) {
        return 0;
    }

    size_t written = 0;
    #define APPEND_REV(data, len) do { \
        if (written + (len) > out_len) return 0; \
        memcpy(out + written, (data), (len)); \
        written += (len); \
    } while (0)

    APPEND_REV(MICHI_HOME_DOMAIN_REVOCATION, strlen(MICHI_HOME_DOMAIN_REVOCATION));
    APPEND_REV(home_id, strlen(home_id));
    APPEND_REV(revoked_device_michi_id, strlen(revoked_device_michi_id));
    APPEND_REV(":", 1);
    APPEND_REV(revoked_at, strlen(revoked_at));
    APPEND_REV(":", 1);
    APPEND_REV(reason, strlen(reason));

    #undef APPEND_REV
    return written;
}

bool michi_home_verify_revocation(
    const michi_revocation_t *revocation,
    const uint8_t root_pk[MICHI_HOME_KEY_BYTES])
{
    if (revocation == NULL || root_pk == NULL) {
        return false;
    }
    if (revocation->version != 1) {
        return false;
    }
    if (s_provisioned && strcmp(s_home_id, revocation->home_id) != 0) {
        return false;
    }

    uint8_t sig[MICHI_HOME_SIG_BYTES];
    size_t sig_len = 0;
    if (michi_identity_base64url_decode(revocation->signature, sig, sizeof(sig), &sig_len) != ESP_OK ||
        sig_len != MICHI_HOME_SIG_BYTES) {
        return false;
    }

    uint8_t canon[512];
    size_t canon_len = michi_home_canonical_revocation_bytes(
        revocation->home_id,
        revocation->revoked_device_michi_id,
        revocation->revoked_at,
        revocation->reason,
        canon,
        sizeof(canon));
    if (canon_len == 0) {
        return false;
    }

    return michi_identity_verify(canon, canon_len, sig, root_pk);
}

esp_err_t michi_home_add_revocation(const michi_revocation_t *revocation)
{
    if (revocation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_provisioned) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!michi_home_verify_revocation(revocation, s_root_pk)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    for (size_t i = 0; i < s_revocation_count; i++) {
        if (strcmp(s_revocations[i].revoked_device_michi_id, revocation->revoked_device_michi_id) == 0) {
            s_revocations[i] = *revocation;
            return ESP_OK;
        }
    }

    if (s_revocation_count >= MICHI_MAX_REVOCATIONS) {
        return ESP_ERR_NO_MEM;
    }

    s_revocations[s_revocation_count++] = *revocation;
    ESP_LOGI(TAG, "Added revocation for device %s", revocation->revoked_device_michi_id);
    return ESP_OK;
}

bool michi_home_is_device_revoked(const char *device_michi_id)
{
    if (device_michi_id == NULL) {
        return false;
    }
    for (size_t i = 0; i < s_revocation_count; i++) {
        if (strcmp(s_revocations[i].revoked_device_michi_id, device_michi_id) == 0) {
            return true;
        }
    }
    return false;
}
