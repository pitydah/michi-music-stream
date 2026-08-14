/*
 * Canonical signed announce builder (MS-05).
 *
 * Pure C (cJSON + michi_identity), no ESP-IDF runtime dependencies: the
 * component and tests/host compile the SAME source. Implements the
 * canonicalization and signing contract of section 2.2 against the
 * golden vectors (contracts/michi-link/vectors/discovery/):
 *
 * - canonical payload: every functional field with lexicographically
 *   ordered keys, signature EXCLUDED (byte-identical to the Rust crate
 *   DiscoveryEngine::canonical_bytes);
 * - signature: Ed25519 over those canonical bytes, base64url-nopad;
 * - wire datagram: ONE compact JSON object (the canonical field set plus
 *   the signature), <= 1200 bytes.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "michi_discovery.h"
#include "michi_identity.h"

/* The feature flags are emitted in lexicographic key order (the crate
 * serializes the BTreeMap the same way) - heartbeat, session, volume. */
static int append_canonical(const michi_discovery_announce_t *a,
                            char *out, size_t out_len)
{
    return snprintf(
        out, out_len,
        "{\"api_version\":\"%s\","
        "\"device_id\":\"%s\","
        "\"features\":{\"heartbeat\":%s,\"session\":%s,\"volume\":%s},"
        "\"host\":\"%s\","
        "\"michi_id\":\"%s\","
        "\"name\":\"%s\","
        "\"nonce\":\"%s\","
        "\"port\":%u,"
        "\"public_key\":\"%s\","
        "\"roles\":[\"%s\"],"
        "\"service\":\"%s\","
        "\"timestamp_ms\":%" PRId64 "}",
        a->api_version, a->device_id,
        a->feature_heartbeat ? "true" : "false",
        a->feature_session ? "true" : "false",
        a->feature_volume ? "true" : "false",
        a->host, a->michi_id, a->name, a->nonce, (unsigned)a->port,
        a->public_key, MICHI_DISCOVERY_ROLE, a->service, a->timestamp_ms);
}

/* Mandatory field validation: every announce string must exist and be
 * non-empty, the port must be a real port. The builder never signs an
 * announce that the schema would reject structurally. */
static bool announce_fields_valid(const michi_discovery_announce_t *a)
{
    return a != NULL && a->device_id != NULL && a->device_id[0] != '\0' &&
           a->name != NULL && a->name[0] != '\0' && a->service != NULL &&
           a->service[0] != '\0' && a->api_version != NULL &&
           a->api_version[0] != '\0' && a->host != NULL &&
           a->host[0] != '\0' && a->michi_id != NULL &&
           a->michi_id[0] != '\0' && a->public_key != NULL &&
           a->public_key[0] != '\0' && a->nonce != NULL &&
           a->nonce[0] != '\0' && a->port >= 1;
}

esp_err_t michi_discovery_canonical_json(const michi_discovery_announce_t *a,
                                         char *out, size_t out_len)
{
    if (!announce_fields_valid(a) || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const int needed = append_canonical(a, out, out_len);
    if (needed < 0 || (size_t)needed >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t michi_discovery_build_announce(const michi_discovery_announce_t *a,
                                         char *out, size_t out_len,
                                         size_t *out_written)
{
    if (out == NULL || out_len == 0 || out_written == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!announce_fields_valid(a)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 1. Canonical payload (what the signature covers). */
    char canonical[MICHI_DISCOVERY_MAX_DATAGRAM_BYTES];
    const esp_err_t c_err =
        michi_discovery_canonical_json(a, canonical, sizeof(canonical));
    if (c_err != ESP_OK) {
        return c_err;
    }

    /* 2. Ed25519 over the canonical bytes (michi_identity, MS-04). */
    uint8_t sig_raw[MICHI_IDENTITY_SIGNATURE_BYTES];
    const esp_err_t s_err = michi_identity_sign(
        (const uint8_t *)canonical, strlen(canonical), sig_raw);
    if (s_err != ESP_OK) {
        return s_err; /* not READY (CORRUPT -> factory reset) */
    }
    char sig_b64[MICHI_IDENTITY_SIGNATURE_B64_LEN];
    const esp_err_t b_err =
        michi_identity_base64url_encode(sig_raw, sizeof(sig_raw), sig_b64,
                                        sizeof(sig_b64));
    if (b_err != ESP_OK) {
        return b_err;
    }

    /* 3. Wire datagram: one compact JSON object with the mandatory
     *    signed group. Built with cJSON (never hand-serialized user data);
     *    the keys follow the canonical order for deterministic wire bytes. */
    cJSON *root = cJSON_CreateObject();
    cJSON *features = NULL;
    cJSON *roles = NULL;
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Every item is attached to the tree as soon as it is created: a
     * failure path only ever deletes root (it owns the whole tree). */
    features = cJSON_AddObjectToObject(root, "features");
    roles = cJSON_AddArrayToObject(root, "roles");
    const bool ok =
        features != NULL && roles != NULL &&
        cJSON_AddItemToArray(roles, cJSON_CreateString(MICHI_DISCOVERY_ROLE)) &&
        cJSON_AddStringToObject(root, "api_version", a->api_version) !=
            NULL &&
        cJSON_AddStringToObject(root, "device_id", a->device_id) != NULL &&
        cJSON_AddBoolToObject(features, "heartbeat", a->feature_heartbeat) !=
            NULL &&
        cJSON_AddBoolToObject(features, "session", a->feature_session) !=
            NULL &&
        cJSON_AddBoolToObject(features, "volume", a->feature_volume) !=
            NULL &&
        cJSON_AddStringToObject(root, "host", a->host) != NULL &&
        cJSON_AddStringToObject(root, "michi_id", a->michi_id) != NULL &&
        cJSON_AddStringToObject(root, "name", a->name) != NULL &&
        cJSON_AddStringToObject(root, "nonce", a->nonce) != NULL &&
        cJSON_AddNumberToObject(root, "port", (double)a->port) != NULL &&
        cJSON_AddStringToObject(root, "public_key", a->public_key) != NULL &&
        cJSON_AddStringToObject(root, "service", a->service) != NULL &&
        cJSON_AddStringToObject(root, "signature", sig_b64) != NULL &&
        cJSON_AddNumberToObject(root, "timestamp_ms",
                                (double)a->timestamp_ms) != NULL;
    if (!ok) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    char *wire = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (wire == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const size_t len = strlen(wire);
    if (len > MICHI_DISCOVERY_MAX_DATAGRAM_BYTES) {
        free(wire);
        return ESP_ERR_INVALID_SIZE; /* contract: one datagram <= 1200 B */
    }
    if (len + 1 > out_len) {
        free(wire);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, wire, len + 1);
    *out_written = len;
    free(wire);
    return ESP_OK;
}
