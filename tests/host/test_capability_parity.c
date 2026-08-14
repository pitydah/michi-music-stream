/* Cross-component capability parity (P0-01 hardening).
 *
 * Compiles the SAME firmware sources the device runs:
 *  - michi_product_profile/capabilities.c (canonical capability flags)
 *  - michi_discovery/announce.c            (signed announce builder)
 *  - michi_http/canonical_json.c           (GET /server/info builder)
 * and proves that the discovery announce and /server/info agree on the
 * common feature subset (session, heartbeat, volume) because both
 * consume the single canonical source - nothing is reimplemented and no
 * capability literal is duplicated here. The announce carries EXACTLY
 * the canonical group {heartbeat, session, volume}; the extended flags
 * (now_playing/diagnostics/ota) exist only in /server/info.
 */

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include "michi_discovery.h"
#include "michi_http.h"
#include "michi_product_profile.h"

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s\n", msg);                                     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static bool json_feature(const cJSON *features, const char *key, bool *out)
{
    const cJSON *v = cJSON_GetObjectItem(features, key);
    if (v == NULL || !cJSON_IsBool(v)) {
        return false;
    }
    *out = cJSON_IsTrue(v);
    return true;
}

static void fill_profile(michi_product_profile_t *p)
{
    memset(p, 0, sizeof(*p));
    p->tier = MICHI_PRODUCT_STANDARD;
    snprintf(p->product_name, sizeof(p->product_name), "%s",
             "Michi Music Stream");
    snprintf(p->firmware_version, sizeof(p->firmware_version), "%s", "0.3.0");
}

static void test_canonical_capability_table(void)
{
    printf("capabilities: the canonical table holds the product truth\n");
    const michi_product_capabilities_t *caps =
        michi_product_profile_capabilities();
    CHECK(caps != NULL, "getter returns the canonical table");
    CHECK(caps->session && caps->heartbeat && caps->volume,
          "session/heartbeat/volume are true (MS-07/MS-08)");
    CHECK(!caps->now_playing, "now_playing is false (501)");
    CHECK(caps->diagnostics, "diagnostics is true");
    CHECK(!caps->ota, "ota is false (501)");
}

/* The announce features built by the REAL announce.c with the flag
 * inputs wired exactly like announce_now_locked() does: from the getter. */
static void test_announce_feature_group(void)
{
    printf("announce: exactly the canonical feature group\n");
    const michi_product_capabilities_t *caps =
        michi_product_profile_capabilities();
    const michi_discovery_announce_t a = {
        .device_id = "550e8400-e29b-41d4-a716-446655440000",
        .name = "Michi Stream Cocina",
        .service = "michi-stream-standard",
        .api_version = "v1-lite",
        .host = "192.168.1.102",
        .port = 8600,
        .feature_session = caps->session,
        .feature_heartbeat = caps->heartbeat,
        .feature_volume = caps->volume,
        .michi_id = "f2UwxQaeA6vA8LO7Cr1nGRr5MStned_Gbmc_ua48qUc",
        .public_key = "RpHnJr9oP1DXBkPuIMuk0hJ2hAJ5SiWO2hAQVCMGREE",
        .timestamp_ms = 1767225600000LL,
        .nonce = "ChEYHyYtNDtCSVBXXmVscw",
    };
    char canonical[512];
    CHECK(michi_discovery_canonical_json(&a, canonical, sizeof(canonical)) ==
              ESP_OK,
          "announce canonical build succeeds");
    cJSON *root = cJSON_Parse(canonical);
    CHECK(root != NULL, "announce parses as JSON");
    if (root == NULL) {
        return;
    }
    cJSON *feat = cJSON_GetObjectItem(root, "features");
    CHECK(feat != NULL && cJSON_IsObject(feat) &&
              cJSON_GetArraySize(feat) == 3,
          "announce features is EXACTLY {heartbeat, session, volume}");
    CHECK(cJSON_GetObjectItem(feat, "now_playing") == NULL &&
              cJSON_GetObjectItem(feat, "diagnostics") == NULL &&
              cJSON_GetObjectItem(feat, "ota") == NULL,
          "extended flags are NOT part of the announce");
    cJSON_Delete(root);
}

static void test_runtime_parity(void)
{
    printf("parity: announce and /server/info agree on the common subset\n");

    /* /server/info: the REAL build_info_json; the capability flags are
     * NOT provided by the caller - they come from the getter. */
    michi_product_profile_t p;
    fill_profile(&p);
    cJSON *info_root = cJSON_CreateObject();
    CHECK(info_root != NULL && build_info_json(info_root, &p) == ESP_OK,
          "server info build succeeds");
    cJSON *info_feat =
        info_root != NULL ? cJSON_GetObjectItem(info_root, "features")
                          : NULL;
    CHECK(info_feat != NULL && cJSON_GetArraySize(info_feat) == 6,
          "server info features is the full 6-flag surface");

    /* Discovery announce: the REAL announce.c wired like the firmware
     * (flags from the getter). */
    const michi_product_capabilities_t *caps =
        michi_product_profile_capabilities();
    const michi_discovery_announce_t a = {
        .device_id = "550e8400-e29b-41d4-a716-446655440000",
        .name = "Michi Stream Cocina",
        .service = "michi-stream-standard",
        .api_version = "v1-lite",
        .host = "192.168.1.102",
        .port = 8600,
        .feature_session = caps->session,
        .feature_heartbeat = caps->heartbeat,
        .feature_volume = caps->volume,
        .michi_id = "f2UwxQaeA6vA8LO7Cr1nGRr5MStned_Gbmc_ua48qUc",
        .public_key = "RpHnJr9oP1DXBkPuIMuk0hJ2hAJ5SiWO2hAQVCMGREE",
        .timestamp_ms = 1767225600000LL,
        .nonce = "ChEYHyYtNDtCSVBXXmVscw",
    };
    char canonical[512];
    CHECK(michi_discovery_canonical_json(&a, canonical, sizeof(canonical)) ==
              ESP_OK,
          "announce canonical build succeeds");
    cJSON *ann_root = cJSON_Parse(canonical);
    CHECK(ann_root != NULL, "announce parses as JSON");

    if (info_feat == NULL || ann_root == NULL) {
        cJSON_Delete(info_root);
        if (ann_root != NULL) {
            cJSON_Delete(ann_root);
        }
        return;
    }
    cJSON *ann_feat = cJSON_GetObjectItem(ann_root, "features");

    bool a_session = false, a_heartbeat = false, a_volume = false;
    bool i_session = false, i_heartbeat = false, i_volume = false;
    CHECK(json_feature(ann_feat, "session", &a_session) &&
              json_feature(ann_feat, "heartbeat", &a_heartbeat) &&
              json_feature(ann_feat, "volume", &a_volume),
          "announce carries the canonical group");
    CHECK(json_feature(info_feat, "session", &i_session) &&
              json_feature(info_feat, "heartbeat", &i_heartbeat) &&
              json_feature(info_feat, "volume", &i_volume),
          "server info carries the common subset");

    CHECK(a_session == i_session, "discovery.features.session == server_info.features.session");
    CHECK(a_heartbeat == i_heartbeat, "discovery.features.heartbeat == server_info.features.heartbeat");
    CHECK(a_volume == i_volume, "discovery.features.volume == server_info.features.volume");
    CHECK(a_session && a_heartbeat && a_volume,
          "the shared subset is advertised true everywhere");

    cJSON_Delete(ann_root);
    cJSON_Delete(info_root);
}

int main(void)
{
    test_canonical_capability_table();
    test_announce_feature_group();
    test_runtime_parity();

    if (failures == 0) {
        printf("test_capability_parity: all tests passed\n");
        return 0;
    }
    printf("test_capability_parity: %d check(s) FAILED\n", failures);
    return 1;
}
