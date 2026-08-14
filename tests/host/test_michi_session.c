/* Host-side tests for the canonical session lifecycle (MS-07).
 *
 * Compiles the REAL firmware source: components/michi_session/
 * michi_session.c against test doubles for the engine (michi_audio_fake),
 * volume, display and the state bus - the REAL michi_audio.h public
 * header is compiled in, so there is no struct drift.
 *
 * Covers (contract section 2.5):
 *  - strict negotiation without clamping (limits rejected, no session);
 *  - one session (second start fails; 409 mapping lives in the HTTP
 *    layer - the layer reports ESP_ERR_INVALID_STATE);
 *  - all-or-nothing start: bind/buffer/audio failures roll back to idle
 *    and a new start succeeds afterwards (no phantom session);
 *  - receiver-picked port 49152..65535, exact negotiated SSRC, HTTP
 *    peer as the only RTP source;
 *  - RAM-only 43-char base64url token (returned once, wiped on stop);
 *  - pause/resume as a state change (engine stays alive);
 *  - volume 0/100 applied;
 *  - stop wipes everything; a new session works after DELETE;
 *  - dead-engine reconciliation;
 *  - metrics mapping (packets_received/rejected/lost/underruns).
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "michi_audio_fake.h"
#include "michi_session.h"
#include "michi_state.h"
#include "michi_volume_fake.h"
#include "michi_display_fake.h"

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s\n", msg);                                     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

#define OWNER "ctrl-1"
#define PEER_IP "192.168.4.20"
#define SSRC 305419896u

static char g_token[MICHI_SESSION_TOKEN_LEN];

static michi_session_start_params_t make_params(void)
{
    const michi_session_start_params_t p = {
        .owner_controller_id = OWNER,
        .codec = "pcm_s16le",
        .sample_rate = 48000,
        .bit_depth = 16,
        .channels = 2,
        .packet_ms = 10,
        .buffer_ms = 120,
        .payload_type = 97,
        .ssrc = SSRC,
        .volume = 70,
        .source_ip = PEER_IP,
    };
    return p;
}

static void reset_all(void)
{
    test_michi_audio_reset();
    test_volume_reset();
    test_display_reset();
    test_state_reset();
}

/* End any active session with the issued token (idempotent helpers). */
static void cleanup_session(void)
{
    if (michi_session_active()) {
        (void)michi_session_stop(g_token);
    }
}

static void test_init_and_token_shape(void)
{
    printf("michi_session: init + token shape\n");
    CHECK(michi_session_init() == ESP_OK, "init ok");
    CHECK(michi_session_init() == ESP_OK, "init idempotent");
    CHECK(michi_session_is_initialized(), "initialized flag");
    CHECK(!michi_session_token_valid(NULL), "NULL token invalid");
    CHECK(!michi_session_token_valid(""), "empty token invalid");
    CHECK(!michi_session_token_valid("short"), "short token invalid");
    /* 43 chars, wrong alphabet (base64 '+' / '=' padding). */
    CHECK(!michi_session_token_valid(
              "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="),
          "padded token invalid");
    CHECK(!michi_session_token_valid(
              "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA+"),
          "non-url alphabet invalid");
    CHECK(michi_session_token_valid(
              "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq"),
          "43-char url-safe token valid");
}

static void test_start_success(void)
{
    printf("michi_session: canonical start\n");
    michi_session_start_params_t p = make_params();
    const esp_err_t err = michi_session_start(&p, g_token, sizeof(g_token));
    CHECK(err == ESP_OK, "start succeeds");
    CHECK(strlen(g_token) == MICHI_SESSION_TOKEN_B64_LEN, "token is 43 chars");
    CHECK(michi_session_token_valid(g_token), "token is base64url-nopad");
    CHECK(michi_session_active(), "session active");

    michi_session_info_t info;
    CHECK(michi_session_get_info(&info) == ESP_OK, "get_info ok");
    CHECK(strlen(info.session_id) == 36, "session_id is a 36-char UUID");
    CHECK(strcmp(info.owner_controller_id, OWNER) == 0, "owner stored");
    CHECK(info.state == MICHI_SESSION_STATE_PLAYING, "state playing");
    CHECK(strcmp(info.codec, "pcm_s16le") == 0, "codec stored");
    CHECK(info.sample_rate == 48000 && info.bit_depth == 16 &&
              info.channels == 2 && info.packet_ms == 10 &&
              info.payload_type == 97,
          "canonical format stored");
    CHECK(info.buffer_ms == 120, "buffer_ms stored (not clamped)");
    CHECK(info.ssrc == SSRC, "negotiated SSRC stored");
    CHECK(strcmp(info.source_addr, PEER_IP) == 0, "source peer stored");
    CHECK(info.stream_port >= 49152 && info.stream_port <= 65535,
          "receiver-picked port in 49152..65535");
    CHECK(info.volume == 70, "applied volume stored");
    CHECK(!info.paused, "not paused");
    CHECK(info.lease_remaining_ms == MICHI_SESSION_LEASE_MS, "lease window");

    /* The engine was asked for the negotiated SSRC + peer, port auto. */
    michi_audio_fake_state_t *fake = test_michi_audio_state();
    CHECK(fake->port_requested == 0, "engine picks the port (port=0)");
    CHECK(fake->ssrc_requested == SSRC, "engine got the exact SSRC");
    CHECK(strcmp(fake->source_ip_requested, PEER_IP) == 0,
          "engine got the HTTP peer IP");

    /* FSM: the SESSION_STARTED chain was posted (3 steps). */
    CHECK(test_state_post_count(MICHI_EVENT_SESSION_STARTED) == 3,
          "FSM chain posted");

    cleanup_session();
}

static void test_second_start_conflict(void)
{
    printf("michi_session: single session rule\n");
    michi_session_start_params_t p = make_params();
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) == ESP_OK,
          "first start ok");
    char first_id[MICHI_SESSION_ID_LEN];
    michi_session_info_t info;
    CHECK(michi_session_get_info(&info) == ESP_OK, "first get_info ok");
    memcpy(first_id, info.session_id, sizeof(first_id));

    char token2[MICHI_SESSION_TOKEN_LEN];
    CHECK(michi_session_start(&p, token2, sizeof(token2)) ==
              ESP_ERR_INVALID_STATE,
          "second start rejected (HTTP: 409)");
    /* The FIRST session is untouched. */
    memset(&info, 0, sizeof(info));
    CHECK(michi_session_get_info(&info) == ESP_OK &&
              strcmp(info.session_id, first_id) == 0 && info.ssrc == SSRC,
          "first session intact after the rejected start");
    cleanup_session();
}

static void test_start_failures_rollback(void)
{
    printf("michi_session: bind/buffer/audio failures roll back\n");
    michi_session_start_params_t p = make_params();

    /* Bind failure. */
    test_michi_audio_set_start_err(ESP_FAIL);
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) == ESP_FAIL,
          "bind failure propagates");
    CHECK(!michi_session_active(), "no session after bind failure");
    CHECK(michi_session_get_info(&(michi_session_info_t){0}) ==
              ESP_ERR_INVALID_STATE,
          "get_info reports no session");
    /* Buffer allocation failure. */
    test_michi_audio_set_start_err(ESP_ERR_NO_MEM);
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) == ESP_ERR_NO_MEM,
          "buffer failure propagates");
    CHECK(!michi_session_active(), "no session after buffer failure");
    /* Audio pipeline failure. */
    test_michi_audio_set_start_err(ESP_ERR_INVALID_STATE);
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) ==
              ESP_ERR_INVALID_STATE,
          "audio failure propagates");
    CHECK(!michi_session_active(), "no session after audio failure");
    /* Rollback is COMPLETE: a new start succeeds right away (no phantom
     * session lingers). */
    test_michi_audio_set_start_err(ESP_OK);
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) == ESP_OK,
          "start succeeds after rollback");
    CHECK(michi_session_active(), "session active after rollback");
    CHECK(test_state_post_count(MICHI_EVENT_SESSION_STARTED) == 3,
          "exactly one successful start chain was posted");
    cleanup_session();
}

static void test_start_rejects_invalid(void)
{
    printf("michi_session: strict negotiation (no clamping)\n");
    michi_session_start_params_t p = make_params();


    p.buffer_ms = 49;
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) ==
              ESP_ERR_INVALID_ARG, "buffer 49 rejected");
    p.buffer_ms = 501;
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) ==
              ESP_ERR_INVALID_ARG, "buffer 501 rejected");
    p.buffer_ms = 120;

    p.ssrc = 0;
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) ==
              ESP_ERR_INVALID_ARG, "ssrc 0 rejected");
    p.ssrc = SSRC;

    p.volume = 101;
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) ==
              ESP_ERR_INVALID_ARG, "volume 101 rejected");
    p.volume = 70;

    p.payload_type = 10;
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) ==
              ESP_ERR_INVALID_ARG, "PT 10 rejected");
    p.payload_type = 97;

    p.packet_ms = 20;
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) ==
              ESP_ERR_INVALID_ARG, "packet_ms 20 rejected");
    p.packet_ms = 10;

    p.codec = "opus";
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) ==
              ESP_ERR_NOT_SUPPORTED, "opus codec rejected");
    p.codec = "pcm_s16le";

    p.sample_rate = 44100;
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) ==
              ESP_ERR_NOT_SUPPORTED, "44100 rejected");
    p.sample_rate = 48000;

    p.source_ip = "10.0.0";
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) ==
              ESP_ERR_INVALID_ARG, "malformed source ip rejected");
    p.source_ip = "999.1.1.1";
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) ==
              ESP_ERR_INVALID_ARG, "out-of-range octet rejected");
    p.source_ip = PEER_IP;

    p.owner_controller_id = NULL;
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) ==
              ESP_ERR_INVALID_ARG, "NULL owner rejected");
    p.owner_controller_id = OWNER;

    char tiny[8];
    CHECK(michi_session_start(&p, tiny, sizeof(tiny)) == ESP_ERR_INVALID_ARG,
          "too-small token buffer rejected");

    CHECK(!michi_session_active(), "no session was left behind");
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) == ESP_OK,
          "valid start still works");
    cleanup_session();
}

static void test_patch(void)
{
    printf("michi_session: patch volume + pause/resume\n");
    michi_session_start_params_t p = make_params();
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) == ESP_OK,
          "start ok");
    michi_session_info_t info;

    /* Volume boundaries: 0 and 100 are APPLIED. */
    CHECK(michi_session_patch(g_token, true, 0, false, false) == ESP_OK,
          "volume 0 patch ok");
    CHECK(michi_session_get_info(&info) == ESP_OK && info.volume == 0,
          "volume 0 applied");
    CHECK(michi_session_patch(g_token, true, 100, false, false) == ESP_OK,
          "volume 100 patch ok");
    CHECK(michi_session_get_info(&info) == ESP_OK && info.volume == 100,
          "volume 100 applied");
    CHECK(michi_session_patch(g_token, true, 101, false, false) ==
              ESP_ERR_INVALID_ARG, "volume 101 rejected (defensive)");

    /* Pause: a STATE change - the engine stays alive. */
    CHECK(michi_session_patch(g_token, false, 0, true, true) == ESP_OK,
          "pause ok");
    CHECK(michi_session_get_info(&info) == ESP_OK && info.paused &&
              info.state == MICHI_SESSION_STATE_PAUSED,
          "state paused");
    CHECK(test_michi_audio_state()->paused, "engine paused");
    CHECK(test_michi_audio_state()->active, "engine task alive while paused");
    CHECK(test_state_post_count(MICHI_EVENT_SESSION_PAUSED) == 1,
          "SESSION_PAUSED posted");

    /* Resume. */
    CHECK(michi_session_patch(g_token, false, 0, true, false) == ESP_OK,
          "resume ok");
    CHECK(michi_session_get_info(&info) == ESP_OK && !info.paused &&
              info.state == MICHI_SESSION_STATE_PLAYING,
          "state playing again");
    CHECK(!test_michi_audio_state()->paused, "engine resumed");
    CHECK(test_state_post_count(MICHI_EVENT_SESSION_RESUMED) == 1,
          "SESSION_RESUMED posted");

    /* Empty patch (no property) is invalid. */
    CHECK(michi_session_patch(g_token, false, 0, false, false) ==
              ESP_ERR_INVALID_ARG, "empty patch rejected");

    /* Wrong / malformed token. */
    CHECK(michi_session_patch("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                              true, 50, false, false) == ESP_ERR_NOT_FOUND,
          "wrong token rejected (HTTP: 401)");
    CHECK(michi_session_patch("not-a-token", true, 50, false, false) ==
              ESP_ERR_INVALID_ARG, "malformed token rejected (HTTP: 401)");

    cleanup_session();
    CHECK(michi_session_patch(g_token, true, 50, false, false) ==
              ESP_ERR_INVALID_STATE, "patch without session (HTTP: 404)");
}

static void test_metrics_mapping(void)
{
    printf("michi_session: info counters from engine metrics\n");
    michi_session_start_params_t p = make_params();
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) == ESP_OK,
          "start ok");

    michi_audio_metrics_t *m = test_michi_audio_metrics();
    m->received = 1250;
    m->lost = 12;
    m->underruns = 3;
    m->drops_malformed = 1;
    m->drops_pt_other = 2;
    m->drops_ssrc_filtered = 4;
    m->drops_source_ip = 8;
    m->drops_payload_geometry = 16;

    michi_session_info_t info;
    CHECK(michi_session_get_info(&info) == ESP_OK, "get_info ok");
    CHECK(info.packets_received == 1250, "packets_received mapped");
    CHECK(info.packets_lost == 12, "packets_lost mapped");
    CHECK(info.underruns == 3, "underruns mapped");
    CHECK(info.packets_rejected == 1 + 2 + 4 + 8 + 16,
          "packets_rejected = sum of the reject classes");
    cleanup_session();
}

static void test_stop_and_new_session(void)
{
    printf("michi_session: stop wipes everything; new session works\n");
    michi_session_start_params_t p = make_params();
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) == ESP_OK,
          "start ok");
    char first_token[MICHI_SESSION_TOKEN_LEN];
    char first_id[MICHI_SESSION_ID_LEN];
    memcpy(first_token, g_token, sizeof(first_token));
    michi_session_info_t info;
    CHECK(michi_session_get_info(&info) == ESP_OK, "get_info ok");
    memcpy(first_id, info.session_id, sizeof(first_id));

    /* Wrong / malformed token. */
    CHECK(michi_session_stop("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") ==
              ESP_ERR_NOT_FOUND, "wrong token rejected (HTTP: 401)");
    CHECK(michi_session_stop("not-a-token") == ESP_ERR_INVALID_ARG,
          "malformed token rejected (HTTP: 401)");
    CHECK(michi_session_active(), "session survives rejected stops");

    /* Real stop: engine teardown + token wiped from RAM. */
    CHECK(michi_session_stop(g_token) == ESP_OK, "stop ok");
    CHECK(!michi_session_active(), "session closed");
    CHECK(michi_session_get_info(&info) == ESP_ERR_INVALID_STATE,
          "get_info: no session (HTTP: 404)");
    CHECK(test_michi_audio_state()->stop_calls == 1, "engine stopped");
    CHECK(test_display_clear_count() == 1, "now-playing cleared");
    CHECK(test_state_post_count(MICHI_EVENT_SESSION_CLOSED) == 1,
          "SESSION_CLOSED posted");
    /* The old token is gone from RAM: stop with it finds NO session. */
    CHECK(michi_session_stop(first_token) == ESP_ERR_INVALID_STATE,
          "old token finds no session (HTTP: 404)");

    /* A new session after DELETE: fresh id and token. */
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) == ESP_OK,
          "new session after DELETE");
    CHECK(strcmp(g_token, first_token) != 0, "fresh token issued");
    CHECK(michi_session_get_info(&info) == ESP_OK &&
              strcmp(info.session_id, first_id) != 0,
          "fresh session id");
    cleanup_session();
}

static void test_dead_engine_reconciliation(void)
{
    printf("michi_session: dead engine reconciliation\n");
    michi_session_start_params_t p = make_params();
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) == ESP_OK,
          "start ok");
    CHECK(michi_session_active(), "session active");
    /* The engine self-terminates (pipeline write rejection). */
    test_michi_audio_state()->active = false;
    michi_session_info_t info;
    CHECK(michi_session_get_info(&info) == ESP_ERR_INVALID_STATE,
          "zombie reported as no session");
    CHECK(!michi_session_active(), "zombie cleaned");
    CHECK(test_state_post_count(MICHI_EVENT_SESSION_CLOSED) == 1,
          "zombie closure posted");
    /* A new start succeeds immediately. */
    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) == ESP_OK,
          "new start after zombie cleanup");
    cleanup_session();
}

static void test_ota_gate_and_abort(void)
{
    printf("michi_session: OTA gate + privileged abort\n");
    test_state_set(MICHI_STATE_UPDATING);
    michi_session_start_params_t p = make_params();

    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) ==
              ESP_ERR_INVALID_STATE, "start rejected while UPDATING");
    test_state_set(MICHI_STATE_IDLE);

    CHECK(michi_session_start(&p, g_token, sizeof(g_token)) == ESP_OK,
          "start ok");
    CHECK(michi_session_abort("ota update") == ESP_OK, "abort ok");
    CHECK(!michi_session_active(), "abort closed the session");
    CHECK(test_state_post_count(MICHI_EVENT_SESSION_CLOSED) == 1,
          "abort posted SESSION_CLOSED");
}

int main(void)
{
    reset_all();
    test_init_and_token_shape();
    test_start_success();
    reset_all();
    test_second_start_conflict();
    reset_all();
    test_start_failures_rollback();
    reset_all();
    test_start_rejects_invalid();
    reset_all();
    test_patch();
    reset_all();
    test_metrics_mapping();
    reset_all();
    test_stop_and_new_session();
    reset_all();
    test_dead_engine_reconciliation();
    reset_all();
    test_ota_gate_and_abort();

    if (failures != 0) {
        printf("michi_session: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("michi_session: all tests passed\n");
    return 0;
}
