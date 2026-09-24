/* Host-side tests for the canonical receiver v1-lite JSON DTO builders
 * (MS-03).
 * Compiles the REAL firmware source: components/michi_http/canonical_json.c
 * (linked from the Makefile) against the SYSTEM cJSON (CI:
 * apt install libcjson-dev) - no reimplementation.
 *
 * Covers the two contract-frozen DTOs:
 *  - michi_http_build_error: the single canonical error envelope
 *    (section 2.7);
 *  - build_info_json: the exact receiver v1-lite info profile
 *    (section 2.1) with truthful feature flags and the certified audio
 *    block.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "michi_http.h"
#include "michi_discovery.h"
#include "michi_identity.h"

static const char *const TEST_STUB_SERVER_ID = "550e8400-e29b-41d4-a716-446655440000";
static const char *const TEST_STUB_MICHI_ID = "f2UwxQaeA6vA8LO7Cr1nGRr5MStned_Gbmc_ua48qUc";
static const char *const TEST_STUB_PUBKEY_B64 = "RpHnJr9oP1DXBkPuIMuk0hJ2hAJ5SiWO2hAQVCMGREE";

esp_err_t michi_discovery_get_server_id(char *out, size_t out_len)
{
    if (out == NULL || out_len < 37) return ESP_ERR_INVALID_SIZE;
    snprintf(out, out_len, "%s", TEST_STUB_SERVER_ID);
    return ESP_OK;
}

esp_err_t michi_identity_michi_id(char *out, size_t out_len)
{
    if (out == NULL || out_len < 44) return ESP_ERR_INVALID_SIZE;
    snprintf(out, out_len, "%s", TEST_STUB_MICHI_ID);
    return ESP_OK;
}

esp_err_t michi_identity_public_key(uint8_t out[32])
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0x42, 32);
    return ESP_OK;
}

esp_err_t michi_identity_base64url_encode(const uint8_t *in, size_t in_len,
                                          char *out, size_t out_len)
{
    (void)in; (void)in_len;
    if (out == NULL || out_len < 44) return ESP_ERR_INVALID_SIZE;
    snprintf(out, out_len, "%s", TEST_STUB_PUBKEY_B64);
    return ESP_OK;
}

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s\n", msg);                                     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

/* ── error envelope (section 2.7) ─────────────────────────── */

static void test_error_envelope(void)
{
    printf("error: canonical envelope\n");
    cJSON *root = NULL;
    esp_err_t err = michi_http_build_error(&root, "INVALID_REQUEST",
                                           "buffer_ms must be between 50 and 500",
                                           "req_test_001", "buffer_ms");
    CHECK(err == ESP_OK, "build_error succeeds");
    CHECK(root != NULL, "root built");

    cJSON *e = cJSON_GetObjectItem(root, "error");
    CHECK(e != NULL && cJSON_IsObject(e), "error object present");

    const cJSON *code = cJSON_GetObjectItem(e, "code");
    CHECK(code != NULL && cJSON_IsString(code) &&
          strcmp(code->valuestring, "INVALID_REQUEST") == 0,
          "code is INVALID_REQUEST");

    const cJSON *msg = cJSON_GetObjectItem(e, "message");
    CHECK(msg != NULL && cJSON_IsString(msg) &&
          strcmp(msg->valuestring,
                 "buffer_ms must be between 50 and 500") == 0,
          "message copied");

    const cJSON *rid = cJSON_GetObjectItem(e, "request_id");
    CHECK(rid != NULL && cJSON_IsString(rid) &&
          strcmp(rid->valuestring, "req_test_001") == 0,
          "request_id copied");

    const cJSON *details = cJSON_GetObjectItem(e, "details");
    CHECK(details != NULL && cJSON_IsObject(details), "details present");
    const cJSON *field = cJSON_GetObjectItem(details, "field");
    CHECK(field != NULL && cJSON_IsString(field) &&
          strcmp(field->valuestring, "buffer_ms") == 0,
          "details.field set");
    CHECK(cJSON_GetArraySize(details) == 1,
          "details has exactly the field entry");

    /* Serialize round-trip: the envelope survives printing/parsing. */
    char *json = cJSON_PrintUnformatted(root);
    CHECK(json != NULL, "envelope serializes");
    if (json != NULL) {
        cJSON *parsed = cJSON_Parse(json);
        CHECK(parsed != NULL, "envelope re-parses");
        if (parsed != NULL) {
            cJSON *pe = cJSON_GetObjectItem(parsed, "error");
            CHECK(pe != NULL &&
                  cJSON_GetObjectItem(pe, "code") != NULL &&
                  cJSON_GetObjectItem(pe, "message") != NULL &&
                  cJSON_GetObjectItem(pe, "request_id") != NULL &&
                  cJSON_GetObjectItem(pe, "details") != NULL,
                  "envelope keys survive round-trip");
            cJSON_Delete(parsed);
        }
        free(json);
    }
    cJSON_Delete(root);
}

static void test_error_envelope_no_field(void)
{
    printf("error: empty details without field\n");
    cJSON *root = NULL;
    esp_err_t err = michi_http_build_error(&root, "NOT_FOUND",
                                           "not found", "req_test_002", NULL);
    CHECK(err == ESP_OK && root != NULL, "build succeeds without field");
    if (root != NULL) {
        cJSON *e = cJSON_GetObjectItem(root, "error");
        const cJSON *details = e != NULL ? cJSON_GetObjectItem(e, "details")
                                         : NULL;
        CHECK(details != NULL && cJSON_IsObject(details) &&
              cJSON_GetArraySize(details) == 0,
              "details present and empty");
        cJSON_Delete(root);
    }
}

static void test_error_envelope_invalid_args(void)
{
    printf("error: invalid args rejected\n");
    cJSON *root = NULL;
    CHECK(michi_http_build_error(NULL, "code", "msg", "req", NULL) != ESP_OK,
          "NULL root rejected");
    CHECK(michi_http_build_error(&root, NULL, "msg", "req", NULL) != ESP_OK,
          "NULL code rejected");
    CHECK(michi_http_build_error(&root, "code", NULL, "req", NULL) != ESP_OK,
          "NULL message rejected");
    CHECK(michi_http_build_error(&root, "code", "msg", NULL, NULL) != ESP_OK,
          "NULL request_id rejected");
    CHECK(root == NULL, "no tree leaked on invalid args");
}

/* ── info profile (section 2.1) ───────────────────────────── */

static void fill_profile(michi_product_profile_t *p, michi_product_tier_t tier,
                         const char *name, const char *version)
{
    memset(p, 0, sizeof(*p));
    p->tier = tier;
    p->audio_available = (tier == MICHI_PRODUCT_HIFI || tier == MICHI_PRODUCT_STANDARD);
    snprintf(p->product_name, sizeof(p->product_name), "%s", name);
    snprintf(p->firmware_version, sizeof(p->firmware_version), "%s", version);
}

static void test_info_profile_standard(void)
{
    printf("info: standard profile exact\n");
    michi_product_profile_t p;
    fill_profile(&p, MICHI_PRODUCT_STANDARD, "Michi Music Stream", "0.3.0");
    cJSON *root = cJSON_CreateObject();
    CHECK(root != NULL, "root created");
    esp_err_t err = build_info_json(root, &p);
    CHECK(err == ESP_OK, "build_info_json succeeds");

    /* Exact top-level key set per server-info.schema.json: 12 keys
     * (service, name, version, api_version, roles, auth, features,
     * server_id, identity_scheme, michi_id, public_key, audio). */
    CHECK(cJSON_GetArraySize(root) == 12, "exactly 12 top-level keys");

    const cJSON *server_id = cJSON_GetObjectItem(root, "server_id");
    CHECK(server_id != NULL && cJSON_IsString(server_id) &&
          strcmp(server_id->valuestring, TEST_STUB_SERVER_ID) == 0,
          "server_id emitted correctly");

    const cJSON *scheme = cJSON_GetObjectItem(root, "identity_scheme");
    CHECK(scheme != NULL && cJSON_IsString(scheme) &&
          strcmp(scheme->valuestring, "ed25519-blake3-v1") == 0,
          "identity_scheme is ed25519-blake3-v1");

    const cJSON *michi_id = cJSON_GetObjectItem(root, "michi_id");
    CHECK(michi_id != NULL && cJSON_IsString(michi_id) &&
          strcmp(michi_id->valuestring, TEST_STUB_MICHI_ID) == 0,
          "michi_id emitted correctly");

    const cJSON *pk = cJSON_GetObjectItem(root, "public_key");
    CHECK(pk != NULL && cJSON_IsString(pk) &&
          strcmp(pk->valuestring, TEST_STUB_PUBKEY_B64) == 0,
          "public_key emitted correctly");

    const cJSON *service = cJSON_GetObjectItem(root, "service");
    CHECK(service != NULL && cJSON_IsString(service) &&
          strcmp(service->valuestring, "michi-stream-standard") == 0,
          "service is michi-stream-standard");

    const cJSON *name = cJSON_GetObjectItem(root, "name");
    CHECK(name != NULL && strcmp(name->valuestring, "Michi Music Stream") == 0,
          "name copied");

    const cJSON *version = cJSON_GetObjectItem(root, "version");
    CHECK(version != NULL && strcmp(version->valuestring, "0.3.0") == 0,
          "version is the firmware version");

    const cJSON *api = cJSON_GetObjectItem(root, "api_version");
    CHECK(api != NULL && strcmp(api->valuestring, "v1-lite") == 0,
          "api_version is v1-lite");

    const cJSON *roles = cJSON_GetObjectItem(root, "roles");
    CHECK(roles != NULL && cJSON_IsArray(roles) &&
          cJSON_GetArraySize(roles) == 1 &&
          strcmp(cJSON_GetArrayItem(roles, 0)->valuestring,
                 "audio_receiver") == 0,
          "roles is exactly [audio_receiver]");

    const cJSON *auth = cJSON_GetObjectItem(root, "auth");
    CHECK(auth != NULL && cJSON_GetArraySize(auth) == 3,
          "auth has exactly 3 keys");
    const cJSON *required = cJSON_GetObjectItem(auth, "required");
    const cJSON *strategy = cJSON_GetObjectItem(auth, "strategy");
    const cJSON *refresh = cJSON_GetObjectItem(auth, "token_refresh");
    CHECK(required != NULL && cJSON_IsTrue(required), "auth.required true");
    CHECK(strategy != NULL && strcmp(strategy->valuestring,
                                     "RECEIVER_BUTTON") == 0,
          "auth.strategy RECEIVER_BUTTON");
    CHECK(refresh != NULL && cJSON_IsFalse(refresh),
          "auth.token_refresh false");

    const cJSON *features = cJSON_GetObjectItem(root, "features");
    CHECK(features != NULL && cJSON_GetArraySize(features) == 6,
          "features has exactly 6 keys");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(features, "session")),
          "features.session true (MS-07, implemented)");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(features, "heartbeat")),
          "features.heartbeat true (MS-08, implemented)");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(features, "volume")),
          "features.volume true (session PATCH, implemented)");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(features, "now_playing")),
          "features.now_playing false (501)");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(features, "diagnostics")),
          "features.diagnostics true (implemented)");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(features, "ota")),
          "features.ota false (501)");

    const cJSON *audio = cJSON_GetObjectItem(root, "audio");
    CHECK(audio != NULL && cJSON_IsObject(audio), "audio object present");
    const cJSON *transports = cJSON_GetObjectItem(audio, "transports");
    const cJSON *codecs = cJSON_GetObjectItem(audio, "codecs");
    const cJSON *rates = cJSON_GetObjectItem(audio, "sample_rates");
    const cJSON *depths = cJSON_GetObjectItem(audio, "bit_depths");
    const cJSON *channels = cJSON_GetObjectItem(audio, "channels");
    const cJSON *packet_ms = cJSON_GetObjectItem(audio, "packet_ms");
    const cJSON *ptypes = cJSON_GetObjectItem(audio, "payload_types");
    CHECK(transports != NULL && cJSON_GetArraySize(transports) == 1 &&
          strcmp(cJSON_GetArrayItem(transports, 0)->valuestring,
                 "rtp_udp") == 0,
          "audio.transports [rtp_udp]");
    CHECK(codecs != NULL && cJSON_GetArraySize(codecs) == 1 &&
          strcmp(cJSON_GetArrayItem(codecs, 0)->valuestring,
                 "pcm_s16le") == 0,
          "audio.codecs [pcm_s16le]");
    CHECK(rates != NULL && cJSON_GetArraySize(rates) == 1 &&
          cJSON_GetArrayItem(rates, 0)->valueint == 48000,
          "audio.sample_rates [48000]");
    CHECK(depths != NULL && cJSON_GetArraySize(depths) == 1 &&
          cJSON_GetArrayItem(depths, 0)->valueint == 16,
          "audio.bit_depths [16]");
    CHECK(channels != NULL && cJSON_GetArraySize(channels) == 1 &&
          cJSON_GetArrayItem(channels, 0)->valueint == 2,
          "audio.channels [2]");
    CHECK(packet_ms != NULL && cJSON_GetArraySize(packet_ms) == 1 &&
          cJSON_GetArrayItem(packet_ms, 0)->valueint == 10,
          "audio.packet_ms [10]");
    CHECK(ptypes != NULL && cJSON_GetArraySize(ptypes) == 1 &&
          cJSON_GetArrayItem(ptypes, 0)->valueint == 97,
          "audio.payload_types [97]");
    CHECK(cJSON_GetObjectItem(audio, "buffer_ms_min") != NULL &&
          cJSON_GetObjectItem(audio, "buffer_ms_min")->valueint == 50,
          "audio.buffer_ms_min 50");
    CHECK(cJSON_GetObjectItem(audio, "buffer_ms_max") != NULL &&
          cJSON_GetObjectItem(audio, "buffer_ms_max")->valueint == 500,
          "audio.buffer_ms_max 500");

    /* Legacy surface must be gone from the profile. */
    CHECK(cJSON_GetObjectItem(root, "device_id") == NULL &&
          cJSON_GetObjectItem(root, "type") == NULL &&
          cJSON_GetObjectItem(root, "firmware") == NULL &&
          cJSON_GetObjectItem(root, "output") == NULL &&
          cJSON_GetObjectItem(root, "supported_codecs") == NULL &&
          cJSON_GetObjectItem(root, "michi_link_version") == NULL,
          "no legacy profile keys");

    char *json = cJSON_PrintUnformatted(root);
    CHECK(json != NULL, "profile serializes");
    if (json != NULL) {
        CHECK(strstr(json, "\"michi-link\"") == NULL,
              "no 'michi-link' service string anywhere in the profile");
        free(json);
    }
    cJSON_Delete(root);
}

static void test_info_profile_hifi(void)
{
    printf("info: hifi service mapping\n");
    michi_product_profile_t p;
    fill_profile(&p, MICHI_PRODUCT_HIFI, "Michi Music Stream HiFi", "0.1.0");
    cJSON *root = cJSON_CreateObject();
    CHECK(root != NULL, "root created");
    CHECK(build_info_json(root, &p) == ESP_OK, "build succeeds");
    const cJSON *service = cJSON_GetObjectItem(root, "service");
    CHECK(service != NULL && strcmp(service->valuestring,
                                    "michi-stream-hifi") == 0,
          "hifi service is michi-stream-hifi");
    cJSON_Delete(root);
}

static void test_info_profile_diagnostic_maps_standard(void)
{
    printf("info: diagnostic tier maps to standard + truthful features\n");
    michi_product_profile_t p;
    fill_profile(&p, MICHI_PRODUCT_DIAGNOSTIC, "Michi Music Stream", "0.1.0");
    cJSON *root = cJSON_CreateObject();
    CHECK(root != NULL, "root created");
    CHECK(build_info_json(root, &p) == ESP_OK, "build succeeds");
    const cJSON *service = cJSON_GetObjectItem(root, "service");
    CHECK(service != NULL && strcmp(service->valuestring,
                                    "michi-stream-standard") == 0,
          "diagnostic service is michi-stream-standard");

    /* Signal Truth (KILLCRITIC P0): in DIAGNOSTIC tier audio_available is false,
     * so session, heartbeat and volume MUST be advertised as false. */
    const cJSON *feat = cJSON_GetObjectItem(root, "features");
    CHECK(feat != NULL, "features present");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(feat, "session")),
          "diagnostic: features.session false");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(feat, "heartbeat")),
          "diagnostic: features.heartbeat false");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(feat, "volume")),
          "diagnostic: features.volume false");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(feat, "diagnostics")),
          "diagnostic: features.diagnostics true");

    cJSON_Delete(root);
}

static void test_info_profile_invalid_args(void)
{
    printf("info: invalid args rejected\n");
    CHECK(build_info_json(NULL, NULL) != ESP_OK, "NULL args rejected");
}

/* Duplicate of request_id_generate to test formatting logic on host */
#include "esp_random.h"
#include <inttypes.h>

static void test_request_id_generate(char *out, size_t out_len)
{
    if (out == NULL || out_len < 37) {
        return;
    }
    const uint32_t a = esp_random();
    const uint32_t b = esp_random();
    const uint32_t c = esp_random();
    const uint32_t d = esp_random();
    const uint32_t e = esp_random();
    /* UUID v4: time_low - time_mid - 4xxx - (10xx variant) - node. */
    snprintf(out, out_len, "%08" PRIx32 "-%04x-4%03x-%04x-%04x%08" PRIx32,
             a,
             (unsigned int)(b & 0xFFFFu),          /* time_mid */
             (unsigned int)((b >> 16) & 0xFFFu),   /* version 4 + time_hi */
             (unsigned int)((c & 0x3FFFu) | 0x8000u), /* variant 10 + clock_seq */
             (unsigned int)(e & 0xFFFFu),          /* node upper 16 bits */
             d);                                   /* node lower 32 bits */
}

static void test_uuid_format(void)
{
    printf("uuid: generator format\n");
    char req_id[37];
    test_request_id_generate(req_id, sizeof(req_id));
    
    CHECK(strlen(req_id) == 36, "length is 36");
    CHECK(req_id[8] == '-', "hyphen 1");
    CHECK(req_id[13] == '-', "hyphen 2");
    CHECK(req_id[18] == '-', "hyphen 3");
    CHECK(req_id[23] == '-', "hyphen 4");
    CHECK(req_id[14] == '4', "version 4");
    
    char variant = req_id[19];
    CHECK(variant == '8' || variant == '9' || variant == 'a' || variant == 'b', "variant RFC 4122");
}

int main(void)
{
    test_error_envelope();
    test_error_envelope_no_field();
    test_error_envelope_invalid_args();
    test_info_profile_standard();
    test_info_profile_hifi();
    test_info_profile_diagnostic_maps_standard();
    test_info_profile_invalid_args();
    test_uuid_format();

    if (failures != 0) {
        printf("\n%d host HTTP/JSON DTO check(s) FAILED\n", failures);
        return 1;
    }
    printf("\nall host HTTP/JSON DTO checks passed\n");
    return 0;
}
