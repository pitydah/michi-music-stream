/*
 * Session lifecycle (MS-07): the single canonical audio session.
 *
 * Design (see include/michi_session.h for the full contract):
 *  - The layer owns the session CONTRACT (strict negotiation without
 *    clamping, single-session rule, the RAM-only session credential,
 *    the FSM lifecycle); the engine (michi_audio) owns the transport.
 *    The API layer (michi_http) is the only caller.
 *  - The session token is 32 CSPRNG bytes encoded base64url WITHOUT
 *    padding (43 chars), issued at start and validated in constant time
 *    (fixed-length XOR loop, single slot). It is never logged, never
 *    persisted, lives ONLY in RAM and is wiped (memset) on stop - the
 *    API returns it once, at creation.
 *  - Format rejection is EXPLICIT and STRICT: anything outside the
 *    canonical negotiation (rtp_udp/pcm_s16le/48000/16/2/10 ms/
 *    50..500 ms/97/ssrc 1..2^32-1/volume 0..100) returns an error with
 *    a log naming the requested values - never silently remapped or
 *    clamped (the HTTP layer answers 400 INVALID_REQUEST with
 *    details.field before this layer is reached).
 *  - Honesty: volume is applied via michi_volume_set() and the APPLIED
 *    value (michi_volume_get) is stored + returned.
 *  - All-or-nothing start: idle -> starting -> engine start (synchronous
 *    socket bind + buffers + task) -> playing. Any engine failure
 *    releases everything and this layer rolls back to idle - a phantom
 *    session cannot exist.
 *  - The stream port is picked by the RECEIVER (the engine binds a free
 *    port in 49152..65535; port 0 request). The RTP source IP is the
 *    HTTP request peer captured by the API layer - never JSON.
 *  - FSM events are best-effort (warn on failure): the session layer is
 *    the API's source of truth; the FSM follows as far as the bus allows.
 *  - No NVS access: sessions are RAM-only by design.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"

#include "michi_audio.h"
#include "michi_display.h"
#include "michi_session.h"
#include "michi_state.h"
#include "michi_volume.h"

#define TAG "michi_session"

/* Canonical negotiation constants (contract section 2.5). */
#define MICHI_SESSION_SAMPLE_RATE 48000
#define MICHI_SESSION_BIT_DEPTH   16
#define MICHI_SESSION_CHANNELS    2
#define MICHI_SESSION_PACKET_MS   10
#define MICHI_SESSION_CODEC       "pcm_s16le"
#define MICHI_SESSION_BUFFER_MIN_MS 50
#define MICHI_SESSION_BUFFER_MAX_MS 500
#define MICHI_SESSION_PAYLOAD_TYPE 97
#define MICHI_SESSION_TOKEN_BYTES 32 /* CSPRNG bytes -> 43 base64url */

typedef struct {
    michi_session_info_t info;
    char session_token[MICHI_SESSION_TOKEN_LEN];
} session_ctx_t;

static SemaphoreHandle_t s_mutex;
static volatile bool s_initialized;
static session_ctx_t s_session;
static bool s_active;

/* ------------------------------------------------------------------
 * Encoding / validation helpers
 * ------------------------------------------------------------------ */

static bool b64url_char_ok(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

bool michi_session_token_valid(const char *token)
{
    if (token == NULL || strlen(token) != MICHI_SESSION_TOKEN_B64_LEN) {
        return false;
    }
    for (size_t i = 0; i < MICHI_SESSION_TOKEN_B64_LEN; i++) {
        if (!b64url_char_ok(token[i])) {
            return false;
        }
    }
    return true;
}

/* RFC 4648 base64url WITHOUT padding: 32 bytes -> 43 chars (no '='). */
static void b64url_encode(const uint8_t *src, size_t len, char *dst)
{
    static const char k_b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t i = 0;
    size_t o = 0;
    while (i + 3 <= len) {
        const uint32_t v = ((uint32_t)src[i] << 16) |
                           ((uint32_t)src[i + 1] << 8) | src[i + 2];
        dst[o++] = k_b64[(v >> 18) & 0x3Fu];
        dst[o++] = k_b64[(v >> 12) & 0x3Fu];
        dst[o++] = k_b64[(v >> 6) & 0x3Fu];
        dst[o++] = k_b64[v & 0x3Fu];
        i += 3;
    }
    if (len - i == 1) {
        const uint32_t v = (uint32_t)src[i] << 16;
        dst[o++] = k_b64[(v >> 18) & 0x3Fu];
        dst[o++] = k_b64[(v >> 12) & 0x3Fu];
    } else if (len - i == 2) {
        const uint32_t v = ((uint32_t)src[i] << 16) |
                           ((uint32_t)src[i + 1] << 8);
        dst[o++] = k_b64[(v >> 18) & 0x3Fu];
        dst[o++] = k_b64[(v >> 12) & 0x3Fu];
        dst[o++] = k_b64[(v >> 6) & 0x3Fu];
    }
    dst[o] = '\0';
}

static void uuid_v4_generate(char *out, size_t out_len)
{
    if (out == NULL || out_len < MICHI_SESSION_ID_LEN) {
        return;
    }
    const uint32_t a = esp_random();
    const uint32_t b = esp_random();
    const uint32_t c = esp_random();
    const uint32_t d = esp_random();
    /* UUID v4: time_low - time_mid - 4xxx - (10xx variant) - node,
     * lowercase hex, the canonical 8-4-4-4-12 grouping. */
    snprintf(out, out_len, "%08" PRIx32 "-%04x-4%03x-%04x-%012" PRIx32,
             a,
             (unsigned int)(b & 0xFFFFu),
             (unsigned int)((b >> 16) & 0xFFFu),
             (unsigned int)((c & 0x3FFFu) | 0x8000u),
             d);
}

static bool ipv4_dotted_valid(const char *s)
{
    if (s == NULL) {
        return false;
    }
    unsigned o[4];
    char tail = 0;
    /* %c catches trailing junk ("1.2.3.4x"); 4 exact octets required. */
    const int n = sscanf(s, "%u.%u.%u.%u%c", &o[0], &o[1], &o[2], &o[3],
                         &tail);
    if (n != 4) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (o[i] > 255) {
            return false;
        }
    }
    return true;
}

/* Fixed-length constant-time comparison: the token is a credential, the
 * comparison time must not depend on the data. Both inputs are exactly
 * MICHI_SESSION_TOKEN_B64_LEN chars (format-validated first); the
 * volatile accumulator forbids short-circuiting. */
static bool token_matches(const char *a, const char *b)
{
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < MICHI_SESSION_TOKEN_B64_LEN; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static bool owner_id_valid(const char *id)
{
    if (id == NULL) {
        return false;
    }
    const size_t len = strlen(id);
    if (len == 0 || len > MICHI_SESSION_OWNER_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const char c = id[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9');
        if (!alnum && c != '-') {
            return false;
        }
    }
    return true;
}

/* Strict negotiation: EXACT canonical values only - explicit rejection
 * with a log naming what was requested, never silent remapping. The
 * HTTP layer already answered 400 for out-of-contract bodies; this gate
 * is the defensive double-check. */
static esp_err_t validate_params(const michi_session_start_params_t *p)
{
    if (!owner_id_valid(p->owner_controller_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (p->codec == NULL || strcmp(p->codec, MICHI_SESSION_CODEC) != 0) {
        ESP_LOGW(TAG, "start: codec '%s' rejected (canonical = %s)",
                 p->codec != NULL ? p->codec : "(null)", MICHI_SESSION_CODEC);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (p->sample_rate != MICHI_SESSION_SAMPLE_RATE) {
        ESP_LOGW(TAG, "start: sample_rate=%" PRIu32 " rejected (canonical "
                      "= %d)",
                 p->sample_rate, MICHI_SESSION_SAMPLE_RATE);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (p->bit_depth != MICHI_SESSION_BIT_DEPTH) {
        ESP_LOGW(TAG, "start: bit_depth=%u rejected (canonical = %d)",
                 (unsigned)p->bit_depth, MICHI_SESSION_BIT_DEPTH);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (p->channels != MICHI_SESSION_CHANNELS) {
        ESP_LOGW(TAG, "start: channels=%u rejected (canonical = %d)",
                 (unsigned)p->channels, MICHI_SESSION_CHANNELS);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (p->packet_ms != MICHI_SESSION_PACKET_MS) {
        ESP_LOGW(TAG, "start: packet_ms=%u rejected (canonical = %d)",
                 (unsigned)p->packet_ms, MICHI_SESSION_PACKET_MS);
        return ESP_ERR_INVALID_ARG;
    }
    if (p->payload_type != MICHI_SESSION_PAYLOAD_TYPE) {
        ESP_LOGW(TAG, "start: payload_type=%u rejected (canonical = %d)",
                 (unsigned)p->payload_type, MICHI_SESSION_PAYLOAD_TYPE);
        return ESP_ERR_INVALID_ARG;
    }
    if (p->buffer_ms < MICHI_SESSION_BUFFER_MIN_MS ||
        p->buffer_ms > MICHI_SESSION_BUFFER_MAX_MS) {
        ESP_LOGW(TAG, "start: buffer_ms=%u rejected (canonical = %d..%d)",
                 (unsigned)p->buffer_ms, MICHI_SESSION_BUFFER_MIN_MS,
                 MICHI_SESSION_BUFFER_MAX_MS);
        return ESP_ERR_INVALID_ARG;
    }
    if (p->ssrc == 0) {
        ESP_LOGW(TAG, "start: ssrc=0 rejected (canonical = 1..4294967295)");
        return ESP_ERR_INVALID_ARG;
    }
    if (p->volume > 100) {
        ESP_LOGW(TAG, "start: volume=%u rejected (canonical = 0..100)",
                 (unsigned)p->volume);
        return ESP_ERR_INVALID_ARG;
    }
    if (!ipv4_dotted_valid(p->source_ip)) {
        ESP_LOGW(TAG, "start: source_ip '%s' is not a dotted IPv4",
                 p->source_ip != NULL ? p->source_ip : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------
 * FSM events (best-effort: the session layer is the source of truth)
 * ------------------------------------------------------------------ */

static const char *event_name(michi_event_id_t id)
{
    switch (id) {
    case MICHI_EVENT_SESSION_STARTED: return "SESSION_STARTED";
    case MICHI_EVENT_SESSION_CLOSED:  return "SESSION_CLOSED";
    case MICHI_EVENT_SESSION_PAUSED:  return "SESSION_PAUSED";
    case MICHI_EVENT_SESSION_RESUMED: return "SESSION_RESUMED";
    default:                          return "EVENT_UNKNOWN";
    }
}

static void post_event(michi_event_id_t id)
{
    const esp_err_t err = michi_state_post(id, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "state_post_failed event=%s err=%s", event_name(id),
                 esp_err_to_name(err));
    }
}

/* Start drives the full chain: idle -> starting -> playing. BUFFERING is
 * modeled retrospectively (like SELF_TEST at boot): the engine's real
 * prefill is internal to michi_audio; the session layer cannot observe
 * it, so the FSM lands on PLAYING once the engine task exists. Each post
 * matches its from-keyed map entry. */
static void post_started_chain(void)
{
    post_event(MICHI_EVENT_SESSION_STARTED);  /* IDLE -> SESSION_PENDING */
    post_event(MICHI_EVENT_SESSION_STARTED);  /* SESSION_PENDING -> BUFFERING */
    post_event(MICHI_EVENT_SESSION_STARTED);  /* BUFFERING -> PLAYING */
}

/* A session whose ENGINE self-terminated (michi_audio_session_active()
 * false: pipeline write rejection inside the engine task - it clears the
 * active flag and exits on its own) is a zombie: the API must never
 * report an active session that cannot stream. Called with s_mutex HELD.
 * A PAUSED session is NOT a zombie (pause keeps the engine task alive).
 * Clears the session state (token wiped from RAM) and posts
 * SESSION_CLOSED (best-effort) so the FSM returns to IDLE. */
static bool session_reconcile_dead_engine_locked(void)
{
    if (!s_active || michi_audio_session_active()) {
        return false;
    }
    ESP_LOGW(TAG, "session: cleaned dead engine session id=%s",
             s_session.info.session_id);
    s_active = false;
    memset(&s_session, 0, sizeof(s_session));
    post_event(MICHI_EVENT_SESSION_CLOSED);
    return true;
}

/* FSM reconciliation: the FSM follows the session layer best-effort - a
 * dropped SESSION_STARTED post (full queue) or a start while the FSM was
 * outside the chain leaves it stuck short of PLAYING. Called on the info
 * path: re-posts the missing chain steps from the CURRENT state. Each
 * post is from-keyed, so a step maps only while the FSM is in the
 * matching state - idempotent, self-healing. States outside the session
 * chain are logged, never forced. */
static void session_reconcile_fsm(void)
{
    const michi_state_t st = michi_state_get();
    if (st == MICHI_STATE_PLAYING || st == MICHI_STATE_PAUSED) {
        return; /* chain complete */
    }
    if (st != MICHI_STATE_IDLE && st != MICHI_STATE_SESSION_PENDING &&
        st != MICHI_STATE_BUFFERING) {
        ESP_LOGW(TAG, "fsm: active session but state=%s is outside the "
                      "session chain (not forced)",
                 michi_state_name(st));
        return;
    }
    if (st == MICHI_STATE_IDLE) {
        post_event(MICHI_EVENT_SESSION_STARTED); /* IDLE -> SESSION_PENDING */
    }
    if (st == MICHI_STATE_IDLE || st == MICHI_STATE_SESSION_PENDING) {
        post_event(MICHI_EVENT_SESSION_STARTED); /* PENDING -> BUFFERING */
    }
    post_event(MICHI_EVENT_SESSION_STARTED);     /* BUFFERING -> PLAYING */
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

esp_err_t michi_session_init(void)
{
    if (s_initialized) {
        return ESP_OK; /* idempotent */
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "init: mutex creation failed");
        return ESP_ERR_NO_MEM;
    }
    s_active = false;
    memset(&s_session, 0, sizeof(s_session));
    s_initialized = true;
    ESP_LOGI(TAG, "subsystem=session state=ok phase=ms07");
    return ESP_OK;
}

esp_err_t michi_session_start(const michi_session_start_params_t *params,
                              char *out_token, size_t out_token_len)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (params == NULL || out_token == NULL ||
        out_token_len < MICHI_SESSION_TOKEN_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t p_err = validate_params(params);
    if (p_err != ESP_OK) {
        return p_err;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* OTA gate (phase 13): while the FSM is UPDATING no new session may
     * start. The HTTP layer answers 409 ota_in_progress BEFORE reaching
     * this call; the gate here is defensive (a race between the busy
     * check and the update teardown). */
    if (michi_state_get() == MICHI_STATE_UPDATING) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "start: rejected state=UPDATING (ota in progress)");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_active) {
        if (session_reconcile_dead_engine_locked()) {
            /* The engine died on its own: the zombie was cleaned above -
             * the new start proceeds below. */
        } else {
            xSemaphoreGive(s_mutex);
            ESP_LOGW(TAG, "start: session %s already active",
                     s_session.info.session_id);
            return ESP_ERR_INVALID_STATE;
        }
    }

    /* idle -> starting: the engine start below is ALL-OR-NOTHING. */
    memset(&s_session, 0, sizeof(s_session));
    s_session.info.state = MICHI_SESSION_STATE_STARTING;

    /* Engine first: synchronous socket bind + buffers + task. Port 0 =
     * the RECEIVER picks a free port in 49152..65535. On ANY failure the
     * engine has released everything it reserved - roll back to idle,
     * never a phantom session. */
    const esp_err_t err = michi_audio_session_start(0, params->ssrc,
                                                    params->source_ip);
    if (err != ESP_OK) {
        memset(&s_session, 0, sizeof(s_session));
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "start: engine rejected the session: %s (rolled back "
                      "to idle)",
                 esp_err_to_name(err));
        return err;
    }
    uint16_t stream_port = 0;
    if (michi_audio_session_get_port(&stream_port) != ESP_OK) {
        /* Defensive: start succeeded but the port is unreadable - tear
         * everything down instead of exposing a broken session. */
        (void)michi_audio_session_stop();
        memset(&s_session, 0, sizeof(s_session));
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "start: engine port unreadable after start - rolled "
                      "back to idle");
        return ESP_ERR_INVALID_STATE;
    }

    /* Apply volume; store the APPLIED value (honesty rule). */
    michi_volume_set(params->volume);
    const uint8_t applied_volume = michi_volume_get();

    /* Session identity: UUID v4 + 32-byte CSPRNG token (base64url-nopad,
     * 43 chars). The credential is returned once and never logged. */
    uint8_t token_bytes[MICHI_SESSION_TOKEN_BYTES];
    esp_fill_random(token_bytes, sizeof(token_bytes));
    b64url_encode(token_bytes, sizeof(token_bytes), s_session.session_token);
    uuid_v4_generate(s_session.info.session_id,
                     sizeof(s_session.info.session_id));

    snprintf(s_session.info.owner_controller_id,
             sizeof(s_session.info.owner_controller_id), "%s",
             params->owner_controller_id);
    strlcpy(s_session.info.codec, params->codec,
            sizeof(s_session.info.codec));
    strlcpy(s_session.info.source_addr, params->source_ip,
            sizeof(s_session.info.source_addr));
    s_session.info.sample_rate = params->sample_rate;
    s_session.info.bit_depth = params->bit_depth;
    s_session.info.channels = params->channels;
    s_session.info.packet_ms = params->packet_ms;
    s_session.info.payload_type = params->payload_type;
    s_session.info.ssrc = params->ssrc;
    s_session.info.stream_port = stream_port;
    s_session.info.buffer_ms = params->buffer_ms;
    s_session.info.volume = applied_volume;
    s_session.info.paused = false;
    s_session.info.lease_remaining_ms = MICHI_SESSION_LEASE_MS;
    s_session.info.state = MICHI_SESSION_STATE_PLAYING;

    s_active = true;
    xSemaphoreGive(s_mutex);

    memcpy(out_token, s_session.session_token, MICHI_SESSION_TOKEN_LEN);
    ESP_LOGI(TAG, "session: started id=%s owner=%s port=%u buffer=%u ms "
                  "volume=%u ssrc=0x%08" PRIx32 " peer=%s",
             s_session.info.session_id,
             s_session.info.owner_controller_id,
             (unsigned)s_session.info.stream_port,
             (unsigned)s_session.info.buffer_ms,
             (unsigned)s_session.info.volume,
             s_session.info.ssrc, s_session.info.source_addr);
    post_started_chain();
    return ESP_OK;
}

esp_err_t michi_session_stop(const char *session_token)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!michi_session_token_valid(session_token)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_active) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (!token_matches(session_token, s_session.session_token)) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "stop: token_mismatch rejected=1");
        return ESP_ERR_NOT_FOUND;
    }

    /* stopping: cooperative engine teardown (socket + buffers + task;
     * may block up to the join window). ESP_ERR_TIMEOUT: the task did
     * not join - nothing is cleared, retry. */
    s_session.info.state = MICHI_SESSION_STATE_STOPPING;
    const esp_err_t err = michi_audio_session_stop();
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "stop: engine did not stop: %s", esp_err_to_name(err));
        return err;
    }

    s_active = false;
    memset(&s_session, 0, sizeof(s_session)); /* token wiped from RAM */
    xSemaphoreGive(s_mutex);

    /* The IDLE screen must never show stale track info. */
    if (michi_display_clear_now_playing() != ESP_OK) {
        ESP_LOGW(TAG, "stop: display clear failed (metadata kept)");
    }

    ESP_LOGI(TAG, "session: stopped");
    post_event(MICHI_EVENT_SESSION_CLOSED);
    return ESP_OK;
}

esp_err_t michi_session_abort(const char *reason)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_active) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    /* Cooperative engine stop, same contract as stop() (no credential:
     * this is the privileged internal path used by michi_ota, which
     * cannot present the never-persisted session token). */
    const esp_err_t err = michi_audio_session_stop();
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "abort: engine did not stop: %s", esp_err_to_name(err));
        return err;
    }

    s_active = false;
    memset(&s_session, 0, sizeof(s_session)); /* token wiped from RAM */
    xSemaphoreGive(s_mutex);

    if (michi_display_clear_now_playing() != ESP_OK) {
        ESP_LOGW(TAG, "abort: display clear failed (metadata kept)");
    }

    ESP_LOGI(TAG, "session: aborted reason=%s", reason != NULL ? reason : "-");
    post_event(MICHI_EVENT_SESSION_CLOSED);
    return ESP_OK;
}

esp_err_t michi_session_patch(const char *session_token, bool volume_set,
                              uint8_t volume, bool paused_set, bool paused)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!michi_session_token_valid(session_token)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (volume_set && volume > 100) {
        return ESP_ERR_INVALID_ARG; /* HTTP validated; defensive */
    }
    if (!volume_set && !paused_set) {
        return ESP_ERR_INVALID_ARG; /* at least one property required */
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_active) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (!token_matches(session_token, s_session.session_token)) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "patch: token_mismatch rejected=1");
        return ESP_ERR_NOT_FOUND;
    }

    if (volume_set) {
        michi_volume_set(volume);
        s_session.info.volume = michi_volume_get(); /* applied value */
    }

    bool state_changed = false;
    if (paused_set && paused != s_session.info.paused) {
        /* Pause/resume is a state change, NOT a teardown: the engine
         * keeps its socket and task alive (valid packets keep being
         * counted and are discarded while paused). */
        michi_audio_session_set_paused(paused);
        s_session.info.paused = paused;
        s_session.info.state = paused ? MICHI_SESSION_STATE_PAUSED
                                      : MICHI_SESSION_STATE_PLAYING;
        state_changed = true;
    }
    xSemaphoreGive(s_mutex);

    if (volume_set) {
        ESP_LOGI(TAG, "session: volume=%u (applied)",
                 (unsigned)s_session.info.volume);
    }
    if (state_changed) {
        post_event(s_session.info.paused ? MICHI_EVENT_SESSION_PAUSED
                                         : MICHI_EVENT_SESSION_RESUMED);
    }
    return ESP_OK;
}

esp_err_t michi_session_get_info(michi_session_info_t *out)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_active) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (session_reconcile_dead_engine_locked()) {
        /* The engine died on its own: the zombie was cleaned above -
         * report the closure as "no session" to the caller. */
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    *out = s_session.info;
    xSemaphoreGive(s_mutex);

    /* Reconcile the FSM chain: an active session must land the FSM on
     * PLAYING/PAUSED; re-post the missing steps from the current state
     * (from-keyed, idempotent). */
    session_reconcile_fsm();

    /* Live counter refresh: the engine metrics are the single source of
     * truth (they survive pause - the task keeps counting). */
    michi_audio_metrics_t m;
    if (michi_audio_get_metrics(&m) == ESP_OK) {
        out->packets_received = m.received;
        out->packets_rejected = m.drops_malformed + m.drops_pt_other +
                                m.drops_ssrc_filtered + m.drops_source_ip +
                                m.drops_payload_geometry;
        out->packets_lost = m.lost;
        out->underruns = m.underruns;
    }
    /* MS-07: the full lease window (heartbeat renewal + the monotonic
     * watchdog land in MS-08; until then the lease never expires). */
    out->lease_remaining_ms = MICHI_SESSION_LEASE_MS;
    return ESP_OK;
}

bool michi_session_active(void)
{
    if (!s_initialized) {
        return false;
    }
    bool active;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    active = s_active;
    xSemaphoreGive(s_mutex);
    return active;
}

bool michi_session_is_initialized(void)
{
    return s_initialized;
}
