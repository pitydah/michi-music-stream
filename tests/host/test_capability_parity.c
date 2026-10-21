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
#include "michi_identity.h"
#include "michi_product_profile.h"
#include "sdkconfig.h"
#include "michi_ops_gate.h"

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

esp_err_t michi_discovery_get_server_id(char *out, size_t out_len)
{
    if (out == NULL || out_len < 37) return ESP_ERR_INVALID_SIZE;
    snprintf(out, out_len, "%s", "550e8400-e29b-41d4-a716-446655440000");
    return ESP_OK;
}

static void fill_profile(michi_product_profile_t *p)
{
    memset(p, 0, sizeof(*p));
    p->tier = MICHI_PRODUCT_STANDARD;
    p->audio_available = true;
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

/* H23-B: "advertised" and "accepted" must be the same predicate.
 *
 * The receiver advertises features.session from the capability table and the
 * HTTP session-create handler gates on michi_product_profile_session_allowed().
 * If those ever diverged, a DAC-less board would publish session=false and
 * still hand out sessions nobody can hear. */
static void test_session_capability_matches_enforcement(void)
{
    printf("capabilities: advertised session capability == session gate\n");

    michi_product_profile_t with_audio;
    fill_profile(&with_audio);
    CHECK(michi_product_profile_session_allowed(&with_audio),
          "audio available -> session gate open");
    CHECK(michi_product_profile_capabilities_for(&with_audio).session,
          "audio available -> session advertised true");

    michi_product_profile_t no_audio;
    fill_profile(&no_audio);
    no_audio.audio_available = false;
    no_audio.tier = MICHI_PRODUCT_DIAGNOSTIC;
    CHECK(!michi_product_profile_session_allowed(&no_audio),
          "no audio (diagnostic board) -> session gate closed");
    CHECK(!michi_product_profile_capabilities_for(&no_audio).session,
          "no audio -> session advertised false");
    CHECK(!michi_product_profile_capabilities_for(&no_audio).volume,
          "no audio -> volume advertised false");
    CHECK(!michi_product_profile_capabilities_for(&no_audio).heartbeat,
          "no audio -> heartbeat advertised false");
    /* Diagnostics must survive: a restricted board is still diagnosable. */
    CHECK(michi_product_profile_capabilities_for(&no_audio).diagnostics,
          "no audio -> diagnostics still advertised true");

    /* No profile at all: never assume audio works. */
    CHECK(!michi_product_profile_session_allowed(NULL),
          "NULL profile -> session gate closed (fail closed)");
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
    michi_identity_init();

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

static void test_host_kconfig_parity(void)
{
    printf("HOST-KCONFIG-01: host sdkconfig.h parity with production defaults\n");
    CHECK(CONFIG_MICHI_DAC_I2C_SDA == 21, "DAC SDA pin == 21");
    CHECK(CONFIG_MICHI_DAC_I2C_SCL == 16, "DAC SCL pin == 16");
    CHECK(CONFIG_MICHI_I2S_BCLK == 3, "I2S BCLK pin == 3");
    CHECK(CONFIG_MICHI_I2S_LRCK == 18, "I2S LRCK pin == 18");
    CHECK(CONFIG_MICHI_I2S_DIN == 5, "I2S DIN pin == 5");
    CHECK(CONFIG_MICHI_I2S_MCLK == -1, "I2S MCLK pin == -1 (disabled/internal PLL)");
    CHECK(CONFIG_MICHI_LED_GPIO == 4, "LED GPIO pin == 4");
    CHECK(CONFIG_MICHI_BUTTON_GPIO == 17, "Button GPIO pin == 17");
    CHECK(CONFIG_MICHI_SKU_EXPECTS_AUDIO == 1, "SKU expects audio == 1");
    CHECK(strcmp(CONFIG_MICHI_DAC_DEFAULT_PROFILE, "pcm5102a") == 0,
          "DAC default profile == pcm5102a");
}

/* H23 Phase 1: a restricted boot with a perfectly good DAC must still publish
 * session=false. This is the contract-facing half of the runtime authority: the
 * capability tables and the REAL /server/info builder, not a reimplementation. */
static void test_restricted_boot_hides_playback(void)
{
    printf("capabilities: restricted boot hides playback despite audio\n");

    michi_product_profile_t p;
    fill_profile(&p); /* audio_available = true */
    CHECK(michi_product_profile_session_allowed(&p), "prerequisite: ready boot allows session");

    CHECK(michi_ops_set(MICHI_OPS_RESTRICTED) == 0, "authority moved to restricted");

    CHECK(!michi_product_profile_session_allowed(&p),
          "H23-RESTRICT-01: session gate closed despite usable audio");
    const michi_product_capabilities_t caps =
        michi_product_profile_capabilities_for(&p);
    CHECK(!caps.session && !caps.heartbeat && !caps.volume,
          "H23-RESTRICT-02: session/heartbeat/volume advertised false");
    CHECK(caps.diagnostics, "H23-RESTRICT-03: diagnostics still advertised");

    /* /server/info is built from the REAL identity-backed builder, so the
     * identity must be initialised first (test_runtime_parity does the same). */
    michi_identity_init();

    /* The published document must carry it, not just the in-memory table. */
    cJSON *info = cJSON_CreateObject();
    CHECK(info != NULL && build_info_json(info, &p) == ESP_OK,
          "H23-RESTRICT-04: /server/info builds while restricted");
    if (info != NULL) {
        const cJSON *feat = cJSON_GetObjectItem(info, "features");
        bool session = true;
        CHECK(feat != NULL && json_feature(feat, "session", &session) && !session,
              "H23-RESTRICT-04: /server/info publishes features.session=false");
        bool diag = false;
        CHECK(feat != NULL && json_feature(feat, "diagnostics", &diag) && diag,
              "H23-RESTRICT-04: /server/info keeps diagnostics=true");
        cJSON_Delete(info);
    }

    /* OTA is refused only while the image state is unprovable or the unit is
     * fatal: a diagnostic board must stay updatable. */
    CHECK(!michi_ops_ota_allowed(), "H23-RESTRICT-06: restricted blocks OTA writes");

    /* Authorised recovery restores service without a reboot. */
    CHECK(michi_ops_recover(MICHI_OPS_READY) == 0, "authorised recovery");
    CHECK(michi_product_profile_session_allowed(&p),
          "H23-RESTRICT-05: recovery re-opens the gate");
    CHECK(michi_ops_ota_allowed(), "H23-RESTRICT-07: ready allows OTA writes");
    CHECK(michi_ops_set(MICHI_OPS_DIAGNOSTIC) == 0, "diagnostic");
    CHECK(michi_ops_ota_allowed(), "H23-RESTRICT-07: diagnostic board stays updatable");
    CHECK(!michi_ops_playback_allowed(), "H23-RESTRICT-07: but it cannot play");

    /* UPDATING: the update owns the device. Playback and further OTA writes are
     * denied while diagnostics stay available, and the authority returns to
     * normal service when the update finishes. */
    CHECK(michi_ops_set(MICHI_OPS_UPDATING) == 0, "H23-UPDATING-01: enter updating");
    CHECK(!michi_ops_playback_allowed(), "H23-UPDATING-02: no playback during an update");
    CHECK(!michi_ops_ota_allowed(), "H23-UPDATING-03: no nested OTA during an update");
    CHECK(!michi_product_profile_session_allowed(&p),
          "H23-UPDATING-04: session gate closed during an update");
    CHECK(michi_product_profile_capabilities_for(&p).diagnostics,
          "H23-UPDATING-05: diagnostics stay advertised");
    CHECK(michi_ops_set(MICHI_OPS_READY) == 0, "H23-UPDATING-06: update finished -> ready");
    CHECK(michi_ops_playback_allowed(), "H23-UPDATING-06: playback resumes after the update");
}

int main(void)
{
    /* H23 Phase 1: capabilities now also depend on the runtime authority, which
     * denies playback until a boot is validated. These suites exercise the
     * advertisement/builders, so they declare a validated boot explicitly. */
    michi_ops_init();
    (void)michi_ops_set(MICHI_OPS_READY);

    test_canonical_capability_table();
    test_session_capability_matches_enforcement();
    test_restricted_boot_hides_playback();
    test_announce_feature_group();
    test_runtime_parity();
    test_host_kconfig_parity();

    if (failures == 0) {
        printf("test_capability_parity: all tests passed\n");
        return 0;
    }
    printf("test_capability_parity: %d check(s) FAILED\n", failures);
    return 1;
}
