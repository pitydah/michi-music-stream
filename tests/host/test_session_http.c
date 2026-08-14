/* Host-side tests for the session/heartbeat HTTP body gates (MS-07/MS-08).
 *
 * Compiles the REAL firmware source: components/michi_http/json_helpers.c
 * (linked from the Makefile) against the SYSTEM cJSON - no
 * reimplementation.
 *
 * Covers receiver-session-create.schema.json /
 * receiver-session-patch.schema.json:
 *  - the canonical create body (spec section 2.5 example);
 *  - the strict limit rejects: buffer_ms 49/501, PT 10/96, SSRC 0 and
 *    4294967296, volume 101/-1, wrong transport/codec/sample_rate/
 *    bit_depth/channels/packet_ms;
 *  - additionalProperties: false - stream_port/source_ip/any extra
 *    property is a 400 with the offending field name (the receiver picks
 *    the port; the RTP source IP is the HTTP request peer - never JSON);
 *  - patch: volume only / paused only / both / empty body / extra field /
 *    wrong types.
 *
 * Covers receiver-heartbeat.schema.json (MS-08):
 *  - the canonical heartbeat body (spec section 2.6 example);
 *  - sequence boundaries 0 / 4294967295 (unsigned, strictly increasing
 *    within the session - the ordering itself lives in michi_session);
 *  - rejects: missing/bad session_id (uuid), missing/negative/
 *    fractional/overflow/string sequence, missing/negative sent_at_ms
 *    (informational), extra properties (additionalProperties false).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "michi_http.h"

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s\n", msg);                                     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static char err_field[32];

#define CANONICAL_CREATE                                                      \
    "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"\
    "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":120,"       \
    "\"payload_type\":97,\"ssrc\":305419896,\"volume\":70}"

static void test_create_valid(void)
{
    printf("session http: canonical create body\n");
    cJSON *root = cJSON_Parse(CANONICAL_CREATE);
    CHECK(root != NULL, "canonical body parses");
    michi_http_session_create_body_t body;
    memset(&body, 0, sizeof(body));
    const bool ok = michi_http_json_get_session_create(
        root, &body, err_field, sizeof(err_field));
    CHECK(ok, "canonical body accepted");
    CHECK(strcmp(body.transport, "rtp_udp") == 0, "transport copied");
    CHECK(strcmp(body.codec, "pcm_s16le") == 0, "codec copied");
    CHECK(body.sample_rate == 48000 && body.bit_depth == 16 &&
              body.channels == 2 && body.packet_ms == 10,
          "const values copied");
    CHECK(body.buffer_ms == 120, "buffer_ms copied");
    CHECK(body.payload_type == 97, "payload_type copied");
    CHECK(body.ssrc == 305419896u, "ssrc copied");
    CHECK(body.volume == 70, "volume copied");
    cJSON_Delete(root);
}

/* Parse + validate a create body; returns true when accepted (err_field
 * only meaningful on rejection). */
static bool try_create(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return false;
    }
    michi_http_session_create_body_t body;
    memset(&body, 0, sizeof(body));
    const bool ok = michi_http_json_get_session_create(
        root, &body, err_field, sizeof(err_field));
    cJSON_Delete(root);
    return ok;
}

static void check_create_reject(const char *name, const char *json,
                                const char *expect_field)
{
    err_field[0] = '\0';
    const bool ok = try_create(json);
    CHECK(!ok, name);
    CHECK(strcmp(err_field, expect_field) == 0,
          "field name"); /* see printf below for the actual value */
    if (strcmp(err_field, expect_field) != 0) {
        printf("      expected field '%s', got '%s'\n", expect_field,
               err_field);
    }
}

static void test_create_rejects(void)
{
    printf("session http: create rejects (limits + const + extra)\n");

    /* buffer_ms limits: 49 / 501 are 400 INVALID_REQUEST. */
    check_create_reject(
        "buffer_ms 49",
        "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
        "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":49,"
        "\"payload_type\":97,\"ssrc\":305419896,\"volume\":70}",
        "buffer_ms");
    check_create_reject(
        "buffer_ms 501",
        "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
        "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":501,"
        "\"payload_type\":97,\"ssrc\":305419896,\"volume\":70}",
        "buffer_ms");
    {
        cJSON *root = cJSON_Parse(
            "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
            "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":50,"
            "\"payload_type\":97,\"ssrc\":305419896,\"volume\":70}");
        michi_http_session_create_body_t body;
        memset(&body, 0, sizeof(body));
        CHECK(root != NULL &&
                  michi_http_json_get_session_create(root, &body, err_field,
                                                     sizeof(err_field)),
              "buffer_ms 50 accepted (boundary)");
        cJSON_Delete(root);
    }
    {
        cJSON *root = cJSON_Parse(
            "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
            "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":500,"
            "\"payload_type\":97,\"ssrc\":305419896,\"volume\":70}");
        michi_http_session_create_body_t body;
        memset(&body, 0, sizeof(body));
        CHECK(root != NULL &&
                  michi_http_json_get_session_create(root, &body, err_field,
                                                     sizeof(err_field)),
              "buffer_ms 500 accepted (boundary)");
        cJSON_Delete(root);
    }

    /* payload_type: PT 10 and 96 are rejected - 97 only. */
    check_create_reject(
        "PT 10",
        "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
        "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":120,"
        "\"payload_type\":10,\"ssrc\":305419896,\"volume\":70}",
        "payload_type");
    check_create_reject(
        "PT 96",
        "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
        "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":120,"
        "\"payload_type\":96,\"ssrc\":305419896,\"volume\":70}",
        "payload_type");

    /* ssrc: 0 and 4294967296 are rejected; 4294967295 accepted. */
    check_create_reject(
        "ssrc 0",
        "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
        "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":120,"
        "\"payload_type\":97,\"ssrc\":0,\"volume\":70}",
        "ssrc");
    check_create_reject(
        "ssrc 4294967296",
        "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
        "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":120,"
        "\"payload_type\":97,\"ssrc\":4294967296,\"volume\":70}",
        "ssrc");
    {
        cJSON *root = cJSON_Parse(
            "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
            "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":120,"
            "\"payload_type\":97,\"ssrc\":4294967295,\"volume\":70}");
        michi_http_session_create_body_t body;
        memset(&body, 0, sizeof(body));
        CHECK(root != NULL &&
                  michi_http_json_get_session_create(root, &body, err_field,
                                                     sizeof(err_field)) &&
                  body.ssrc == 4294967295u,
              "ssrc 4294967295 accepted");
        cJSON_Delete(root);
    }

    /* volume 0/100 accepted (boundaries); 101/-1 rejected. */
    {
        cJSON *root = cJSON_Parse(
            "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
            "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":120,"
            "\"payload_type\":97,\"ssrc\":305419896,\"volume\":0}");
        michi_http_session_create_body_t body;
        memset(&body, 0, sizeof(body));
        CHECK(root != NULL &&
                  michi_http_json_get_session_create(root, &body, err_field,
                                                     sizeof(err_field)),
              "volume 0 accepted");
        cJSON_Delete(root);
    }
    {
        cJSON *root = cJSON_Parse(
            "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
            "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":120,"
            "\"payload_type\":97,\"ssrc\":305419896,\"volume\":100}");
        michi_http_session_create_body_t body;
        memset(&body, 0, sizeof(body));
        CHECK(root != NULL &&
                  michi_http_json_get_session_create(root, &body, err_field,
                                                     sizeof(err_field)),
              "volume 100 accepted");
        cJSON_Delete(root);
    }
    check_create_reject(
        "volume 101",
        "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
        "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":120,"
        "\"payload_type\":97,\"ssrc\":305419896,\"volume\":101}",
        "volume");
    check_create_reject(
        "volume -1",
        "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
        "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":120,"
        "\"payload_type\":97,\"ssrc\":305419896,\"volume\":-1}",
        "volume");

    /* const fields: transport/codec/sample_rate/bit_depth/channels/
     * packet_ms are EXACT - anything else is 400. */
    check_create_reject(
        "transport tcp",
        "{\"transport\":\"tcp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
        "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":120,"
        "\"payload_type\":97,\"ssrc\":305419896,\"volume\":70}",
        "transport");
    check_create_reject(
        "codec opus",
        "{\"transport\":\"rtp_udp\",\"codec\":\"opus\",\"sample_rate\":48000,"
        "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":120,"
        "\"payload_type\":97,\"ssrc\":305419896,\"volume\":70}",
        "codec");
    check_create_reject(
        "sample_rate 44100",
        "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":44100,"
        "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":120,"
        "\"payload_type\":97,\"ssrc\":305419896,\"volume\":70}",
        "sample_rate");
    check_create_reject(
        "bit_depth 24",
        "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
        "\"bit_depth\":24,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":120,"
        "\"payload_type\":97,\"ssrc\":305419896,\"volume\":70}",
        "bit_depth");
    check_create_reject(
        "channels 1",
        "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
        "\"bit_depth\":16,\"channels\":1,\"packet_ms\":10,\"buffer_ms\":120,"
        "\"payload_type\":97,\"ssrc\":305419896,\"volume\":70}",
        "channels");
    check_create_reject(
        "packet_ms 20",
        "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
        "\"bit_depth\":16,\"channels\":2,\"packet_ms\":20,\"buffer_ms\":120,"
        "\"payload_type\":97,\"ssrc\":305419896,\"volume\":70}",
        "packet_ms");

    /* Missing fields are rejected with the missing field name. */
    check_create_reject(
        "missing buffer_ms",
        "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
        "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,"
        "\"payload_type\":97,\"ssrc\":305419896,\"volume\":70}",
        "buffer_ms");
    check_create_reject(
        "missing ssrc",
        "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
        "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":120,"
        "\"payload_type\":97,\"volume\":70}",
        "ssrc");

    /* Wrong types are rejected with the field name (no coercion). */
    check_create_reject(
        "string ssrc",
        "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
        "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":120,"
        "\"payload_type\":97,\"ssrc\":\"305419896\",\"volume\":70}",
        "ssrc");
    check_create_reject(
        "fractional volume",
        "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
        "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":120,"
        "\"payload_type\":97,\"ssrc\":305419896,\"volume\":70.5}",
        "volume");

    /* additionalProperties: false - the receiver picks the stream port
     * and the RTP source IP is the HTTP request peer, NEVER JSON. */
    check_create_reject(
        "extra stream_port",
        "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
        "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":120,"
        "\"payload_type\":97,\"ssrc\":305419896,\"volume\":70,"
        "\"stream_port\":55300}",
        "stream_port");
    check_create_reject(
        "extra source_ip",
        "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
        "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":120,"
        "\"payload_type\":97,\"ssrc\":305419896,\"volume\":70,"
        "\"source_ip\":\"192.168.1.5\"}",
        "source_ip");
    check_create_reject(
        "extra unknown",
        "{\"transport\":\"rtp_udp\",\"codec\":\"pcm_s16le\",\"sample_rate\":48000,"
        "\"bit_depth\":16,\"channels\":2,\"packet_ms\":10,\"buffer_ms\":120,"
        "\"payload_type\":97,\"ssrc\":305419896,\"volume\":70,\"foo\":1}",
        "foo");
}

static bool try_patch(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return false;
    }
    michi_http_session_patch_body_t body;
    memset(&body, 0, sizeof(body));
    const bool ok = michi_http_json_get_session_patch(
        root, &body, err_field, sizeof(err_field));
    cJSON_Delete(root);
    return ok;
}

static void check_patch_reject(const char *name, const char *json,
                               const char *expect_field)
{
    err_field[0] = '\0';
    CHECK(!try_patch(json), name);
    CHECK(strcmp(err_field, expect_field) == 0, "field name");
    if (strcmp(err_field, expect_field) != 0) {
        printf("      expected field '%s', got '%s'\n", expect_field,
               err_field);
    }
}

static void test_patch(void)
{
    printf("session http: patch body gates\n");

    {
        cJSON *root = cJSON_Parse("{\"volume\":55}");
        michi_http_session_patch_body_t body;
        memset(&body, 0, sizeof(body));
        CHECK(root != NULL &&
                  michi_http_json_get_session_patch(root, &body, err_field,
                                                    sizeof(err_field)) &&
                  body.has_volume && body.volume == 55 && !body.has_paused,
              "volume-only patch accepted");
        cJSON_Delete(root);
    }
    {
        cJSON *root = cJSON_Parse("{\"paused\":true}");
        michi_http_session_patch_body_t body;
        memset(&body, 0, sizeof(body));
        CHECK(root != NULL &&
                  michi_http_json_get_session_patch(root, &body, err_field,
                                                    sizeof(err_field)) &&
                  body.has_paused && body.paused && !body.has_volume,
              "paused-only patch accepted");
        cJSON_Delete(root);
    }
    {
        cJSON *root = cJSON_Parse("{\"volume\":0,\"paused\":true}");
        michi_http_session_patch_body_t body;
        memset(&body, 0, sizeof(body));
        CHECK(root != NULL &&
                  michi_http_json_get_session_patch(root, &body, err_field,
                                                    sizeof(err_field)) &&
                  body.has_volume && body.volume == 0 && body.has_paused,
              "volume+paused patch accepted");
        cJSON_Delete(root);
    }
    {
        cJSON *root = cJSON_Parse("{\"volume\":100}");
        michi_http_session_patch_body_t body;
        memset(&body, 0, sizeof(body));
        CHECK(root != NULL &&
                  michi_http_json_get_session_patch(root, &body, err_field,
                                                    sizeof(err_field)),
              "volume 100 accepted (boundary)");
        cJSON_Delete(root);
    }

    check_patch_reject("empty patch", "{}", "body");
    check_patch_reject("volume 101", "{\"volume\":101}", "volume");
    check_patch_reject("volume -1", "{\"volume\":-1}", "volume");
    check_patch_reject("string volume", "{\"volume\":\"55\"}", "volume");
    check_patch_reject("string paused", "{\"paused\":\"yes\"}", "paused");
    check_patch_reject("extra field", "{\"paused\":true,\"volume\":1,"
                                      "\"stream_port\":9}",
                       "stream_port");
    check_patch_reject("extra field 2", "{\"state\":\"playing\"}", "state");
}

/* Parse + validate a heartbeat body; returns true when accepted. */
static bool try_heartbeat(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return false;
    }
    michi_http_heartbeat_body_t body;
    memset(&body, 0, sizeof(body));
    const bool ok = michi_http_json_get_heartbeat(root, &body, err_field,
                                                  sizeof(err_field));
    cJSON_Delete(root);
    return ok;
}

static void check_heartbeat_reject(const char *name, const char *json,
                                   const char *expect_field)
{
    err_field[0] = '\0';
    const bool ok = try_heartbeat(json);
    CHECK(!ok, name);
    CHECK(strcmp(err_field, expect_field) == 0, "field name");
    if (strcmp(err_field, expect_field) != 0) {
        printf("      expected field '%s', got '%s'\n", expect_field,
               err_field);
    }
}

static void test_heartbeat(void)
{
    printf("session http: heartbeat body gates (MS-08)\n");

    /* The canonical heartbeat (spec section 2.6 example). */
    {
        cJSON *root = cJSON_Parse(
            "{\"session_id\":\"550e8400-e29b-41d4-a716-446655440003\","
            "\"sequence\":7,\"sent_at_ms\":1786564800000}");
        michi_http_heartbeat_body_t body;
        memset(&body, 0, sizeof(body));
        CHECK(root != NULL &&
                  michi_http_json_get_heartbeat(root, &body, err_field,
                                                sizeof(err_field)),
              "canonical heartbeat accepted");
        CHECK(strcmp(body.session_id,
                     "550e8400-e29b-41d4-a716-446655440003") == 0,
              "session_id copied");
        CHECK(body.sequence == 7, "sequence copied");
        CHECK(body.sent_at_ms == 1786564800000LL, "sent_at_ms copied "
              "(informational)");
        cJSON_Delete(root);
    }
    /* Boundaries: sequence 0 and the unsigned maximum are valid. */
    {
        cJSON *root = cJSON_Parse(
            "{\"session_id\":\"550e8400-e29b-41d4-a716-446655440003\","
            "\"sequence\":0,\"sent_at_ms\":0}");
        michi_http_heartbeat_body_t body;
        memset(&body, 0, sizeof(body));
        CHECK(root != NULL &&
                  michi_http_json_get_heartbeat(root, &body, err_field,
                                                sizeof(err_field)),
              "sequence 0 accepted (first heartbeat)");
        cJSON_Delete(root);
    }
    {
        cJSON *root = cJSON_Parse(
            "{\"session_id\":\"550e8400-e29b-41d4-a716-446655440003\","
            "\"sequence\":4294967295,\"sent_at_ms\":0}");
        michi_http_heartbeat_body_t body;
        memset(&body, 0, sizeof(body));
        CHECK(root != NULL &&
                  michi_http_json_get_heartbeat(root, &body, err_field,
                                                sizeof(err_field)),
              "sequence 4294967295 accepted (unsigned max)");
        cJSON_Delete(root);
    }

    check_heartbeat_reject("missing session_id",
                           "{\"sequence\":7,\"sent_at_ms\":1}",
                           "session_id");
    check_heartbeat_reject("bad session_id uuid",
                           "{\"session_id\":\"nope\",\"sequence\":7,"
                           "\"sent_at_ms\":1}",
                           "session_id");
    check_heartbeat_reject("missing sequence",
                           "{\"session_id\":\"550e8400-e29b-41d4-a716-"
                           "446655440003\",\"sent_at_ms\":1}",
                           "sequence");
    check_heartbeat_reject("negative sequence",
                           "{\"session_id\":\"550e8400-e29b-41d4-a716-"
                           "446655440003\",\"sequence\":-1,\"sent_at_ms\":1}",
                           "sequence");
    check_heartbeat_reject("fractional sequence",
                           "{\"session_id\":\"550e8400-e29b-41d4-a716-"
                           "446655440003\",\"sequence\":1.5,\"sent_at_ms\":1}",
                           "sequence");
    check_heartbeat_reject("sequence overflow",
                           "{\"session_id\":\"550e8400-e29b-41d4-a716-"
                           "446655440003\",\"sequence\":4294967296,"
                           "\"sent_at_ms\":1}",
                           "sequence");
    check_heartbeat_reject("string sequence",
                           "{\"session_id\":\"550e8400-e29b-41d4-a716-"
                           "446655440003\",\"sequence\":\"7\","
                           "\"sent_at_ms\":1}",
                           "sequence");
    check_heartbeat_reject("missing sent_at_ms",
                           "{\"session_id\":\"550e8400-e29b-41d4-a716-"
                           "446655440003\",\"sequence\":7}",
                           "sent_at_ms");
    check_heartbeat_reject("negative sent_at_ms",
                           "{\"session_id\":\"550e8400-e29b-41d4-a716-"
                           "446655440003\",\"sequence\":7,\"sent_at_ms\":-1}",
                           "sent_at_ms");
    check_heartbeat_reject("extra field",
                           "{\"session_id\":\"550e8400-e29b-41d4-a716-"
                           "446655440003\",\"sequence\":7,"
                           "\"sent_at_ms\":1,\"token\":\"x\"}",
                           "token");
}

int main(void)
{
    test_create_valid();
    test_create_rejects();
    test_patch();
    test_heartbeat();
    if (failures != 0) {
        printf("session_http: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("session_http: all tests passed\n");
    return 0;
}
