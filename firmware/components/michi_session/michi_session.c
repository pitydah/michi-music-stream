/*
 * Session lifecycle (phase 12): the single active audio session.
 *
 * Design (see include/michi_session.h for the full contract):
 *  - The layer owns the session CONTRACT (format validation vs profile /
 *    engine meta 1, single-session rule, the session credential, the FSM
 *    lifecycle); the engine (michi_audio) owns the transport. The API
 *    layer (michi_http) is the only caller in phase 12.
 *  - The session token is a random 64-hex credential issued at start and
 *    validated in constant time (fixed-length XOR loop, single slot -
 *    there is nothing to hide about WHICH slot matched). It is never
 *    logged, never persisted; the API returns it once, at creation.
 *  - Format rejection is EXPLICIT: anything outside meta 1
 *    (pcm_s16le/48000/16/2) returns ESP_ERR_NOT_SUPPORTED with a log
 *    naming the requested values - never silently remapped.
 *  - Honesty rules (shared with P0-12): buffer_ms is clamped to the
 *    engine's jitter capacity and the CLAMPED value is stored + returned;
 *    volume is clamped and the APPLIED value (michi_volume_get) is
 *    stored + returned.
 *  - FSM events are best-effort (warn on failure): the session layer is
 *    the API's source of truth; the FSM follows as far as the bus allows.
 *  - No NVS access: sessions are RAM-only by design.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "michi_audio.h"
#include "michi_display.h"
#include "michi_product_profile.h"
#include "michi_session.h"
#include "michi_state.h"
#include "michi_volume.h"

#define TAG "michi_session"

/* Meta 1 validated baseline (engine constants mirror michi_audio). */
#define MICHI_SESSION_SAMPLE_RATE 48000
#define MICHI_SESSION_BIT_DEPTH   16
#define MICHI_SESSION_CHANNELS    2
#define MICHI_SESSION_CODEC       "pcm_s16le"

#define MICHI_SESSION_PORT_MIN 1024 /* upper bound 65535 is the uint16_t type limit */
#define MICHI_SESSION_BUFFER_MIN_MS 50

#define MICHI_SESSION_ID_RANDOM_BYTES 8     /* 16 hex chars */
#define MICHI_SESSION_TOKEN_RANDOM_BYTES 32 /* 64 hex chars */

/* The engine task binds its socket asynchronously after
 * michi_audio_session_start() returns (it preempts the httpd task at the
 * next tick). Reconciliation treats "engine inactive right after start"
 * as a still-booting engine, not a dead one: this grace window covers
 * the bind race (a few ms worst case) before a zombie can be declared. */
#define MICHI_SESSION_ENGINE_GRACE_MS 250

typedef struct {
    michi_session_info_t info;
    char session_token[MICHI_SESSION_TOKEN_LEN];
} session_ctx_t;

static SemaphoreHandle_t s_mutex;
static volatile bool s_initialized;
static session_ctx_t s_session;
static bool s_active;
/* esp_timer timestamp of the last session start: the dead-engine
 * reconciliation grace window (the engine binds its socket
 * asynchronously after michi_audio_session_start() returns). */
static int64_t s_session_start_us;

/* ------------------------------------------------------------------
 * Hex helpers (format validation: the token/ids are hex strings)
 * ------------------------------------------------------------------ */

static bool hex_char_ok(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static bool hex_string_ok(const char *s, size_t expect_len)
{
    if (s == NULL || strlen(s) != expect_len) {
        return false;
    }
    for (size_t i = 0; i < expect_len; i++) {
        if (!hex_char_ok(s[i])) {
            return false;
        }
    }
    return true;
}

static void hex_encode(const uint8_t *src, size_t len, char *dst)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        dst[2 * i] = digits[src[i] >> 4];
        dst[2 * i + 1] = digits[src[i] & 0x0f];
    }
    dst[2 * len] = '\0';
}

/* ------------------------------------------------------------------
 * Session token validation (constant time, single slot)
 * ------------------------------------------------------------------ */

/* Fixed-length constant-time comparison: the token is a credential, the
 * comparison time must not depend on the data. Both inputs are exactly
 * MICHI_SESSION_TOKEN_HEX_LEN chars (format-validated first); the
 * volatile accumulator forbids short-circuiting. */
static bool token_matches(const char *a, const char *b)
{
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < MICHI_SESSION_TOKEN_HEX_LEN; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
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

/* Start drives the full chain: negotiated -> engine starting -> running.
 * BUFFERING is modeled retrospectively (like SELF_TEST at boot): the
 * engine's real prefill is internal to michi_audio; the session layer
 * cannot observe it, so the FSM lands on PLAYING once the engine task
 * was created. Each post matches its from-keyed map entry. */
static void post_started_chain(void)
{
    post_event(MICHI_EVENT_SESSION_STARTED);  /* IDLE -> SESSION_PENDING */
    post_event(MICHI_EVENT_SESSION_STARTED);  /* SESSION_PENDING -> BUFFERING */
    post_event(MICHI_EVENT_SESSION_STARTED);  /* BUFFERING -> PLAYING */
}

/* A session whose ENGINE self-terminated (michi_audio_session_active()
 * false: socket/bind failure or a pipeline write rejection inside the
 * engine task - it clears the active flag and exits on its own) is a
 * zombie: the API must never report an active session that cannot
 * stream. Called with s_mutex HELD. A PAUSED session is NOT a zombie
 * (pause stops the engine by design). The grace window after start
 * covers the async socket bind race. Clears the session state and posts
 * SESSION_CLOSED (best-effort) so the FSM returns to IDLE. */
static bool session_reconcile_dead_engine_locked(void)
{
    if (!s_active || s_session.info.paused) {
        return false;
    }
    if (michi_audio_session_active()) {
        return false;
    }
    const uint32_t age_ms =
        (uint32_t)((esp_timer_get_time() - s_session_start_us) / 1000);
    if (age_ms < MICHI_SESSION_ENGINE_GRACE_MS) {
        return false; /* engine still booting (async bind) */
    }
    ESP_LOGW(TAG, "session: cleaned dead engine session id=%s",
             s_session.info.session_id);
    s_active = false;
    memset(&s_session, 0, sizeof(s_session));
    post_event(MICHI_EVENT_SESSION_CLOSED);
    return true;
}

/* FSM reconciliation (F8): the FSM follows the session layer best-effort
 * - a dropped SESSION_STARTED post (full queue) or a start while the FSM
 * was outside the chain leaves it stuck short of PLAYING. Called on the
 * info path: re-posts the missing chain steps from the CURRENT state.
 * Each post is from-keyed, so a step maps only while the FSM is in the
 * matching state - idempotent, self-healing (a later get_info retries
 * whatever still did not map). States outside the session chain are
 * logged, never forced. */
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
    post_event(MICHI_EVENT_SESSION_STARTED);      /* BUFFERING -> PLAYING */
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
    s_session_start_us = 0;
    s_initialized = true;
    ESP_LOGI(TAG, "subsystem=session state=ok phase=12");
    return ESP_OK;
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

/* Explicit format rejection: the codec must be in the product profile's
 * supported_codecs AND the meta-1 string; the sample format must pass
 * the engine's own prepare(). NOT_SUPPORTED with a log naming what was
 * requested - the API layer surfaces it as a 400. */
static esp_err_t validate_format(const char *codec, uint32_t sample_rate,
                                 uint8_t bit_depth, uint8_t channels)
{
    const michi_product_profile_t *p = michi_product_profile_get();
    bool codec_known = false;
    for (uint8_t i = 0; i < p->supported_codecs_count; i++) {
        if (strcmp(codec, p->supported_codecs[i]) == 0) {
            codec_known = true;
            break;
        }
    }
    if (!codec_known) {
        ESP_LOGW(TAG, "start: codec '%s' rejected - supported: %s%s%s",
                 codec, p->supported_codecs[0],
                 p->supported_codecs_count > 1 ? ", " : "",
                 p->supported_codecs_count > 1 ? p->supported_codecs[1] : "");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (strcmp(codec, MICHI_SESSION_CODEC) != 0) {
        ESP_LOGW(TAG, "start: codec '%s' is declared but NOT implemented in "
                      "phase 12 (meta 1 = %s) - rejected explicitly",
                 codec, MICHI_SESSION_CODEC);
        return ESP_ERR_NOT_SUPPORTED;
    }
    const michi_audio_output_ops_t *ops = michi_audio_get_output_ops();
    const esp_err_t err = ops->prepare(sample_rate, bit_depth, channels);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start: format %" PRIu32 "/%u/%u rejected (phase 12 "
                      "supports %d/%d/%d)",
                 sample_rate, bit_depth, channels, MICHI_SESSION_SAMPLE_RATE,
                 MICHI_SESSION_BIT_DEPTH, MICHI_SESSION_CHANNELS);
    }
    return err;
}

esp_err_t michi_session_start(const char *owner_controller_id,
                              const char *codec,
                              uint32_t sample_rate, uint8_t bit_depth,
                              uint8_t channels, uint16_t stream_port,
                              uint16_t buffer_ms, uint8_t volume,
                              char *out_token, size_t out_token_len)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (codec == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_token == NULL || out_token_len < MICHI_SESSION_TOKEN_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!owner_id_valid(owner_controller_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    /* stream_port is uint16_t: only the lower bound needs checking (the
     * upper bound 65535 cannot be exceeded by the type). */
    if (stream_port < MICHI_SESSION_PORT_MIN) {
        ESP_LOGW(TAG, "start: stream_port=%u below %d",
                 (unsigned)stream_port, MICHI_SESSION_PORT_MIN);
        return ESP_ERR_INVALID_ARG;
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
            /* The engine died on its own (F2): the zombie was cleaned
             * above - the new start proceeds below. */
        } else {
            xSemaphoreGive(s_mutex);
            ESP_LOGW(TAG, "start: session %s already active",
                     s_session.info.session_id);
            return ESP_ERR_INVALID_STATE;
        }
    }
    /* The active check comes BEFORE format validation (F5): a live
     * session answers 409 session_active with precedence, as the header
     * documents. validate_format is pure (no locks, no blocking), so it
     * is safe under the mutex. */
    const esp_err_t fmt_err = validate_format(codec, sample_rate, bit_depth,
                                              channels);
    if (fmt_err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return fmt_err;
    }

    /* Engine first: the task binds the port. INVALID_STATE here means the
     * audio pipeline is not running (no DAC detected at boot). */
    const esp_err_t err = michi_audio_session_start(stream_port, 0);
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "start: engine rejected the session: %s",
                 esp_err_to_name(err));
        return err;
    }

    /* Clamp + apply volume; store the APPLIED value (P0-12). */
    uint8_t v = volume;
    if (v > 100) {
        v = 100;
    }
    michi_volume_set(v);
    const uint8_t applied_volume = michi_volume_get();

    /* Clamp buffer_ms to the engine's jitter capacity; store the CLAMPED
     * value (reported = effective, P0-12). */
    uint16_t effective_ms = buffer_ms;
    if (effective_ms < MICHI_SESSION_BUFFER_MIN_MS) {
        effective_ms = MICHI_SESSION_BUFFER_MIN_MS;
    }
    if (effective_ms > CONFIG_MICHI_AUDIO_JITTER_MAX_MS) {
        ESP_LOGW(TAG, "start: buffer_ms=%u clamped to engine capacity %d ms",
                 (unsigned)buffer_ms, CONFIG_MICHI_AUDIO_JITTER_MAX_MS);
        effective_ms = (uint16_t)CONFIG_MICHI_AUDIO_JITTER_MAX_MS;
    }

    /* Session identity: id (8 random bytes hex) + credential (32 random
     * bytes hex). The credential is returned once and never logged. */
    uint8_t id_bytes[MICHI_SESSION_ID_RANDOM_BYTES];
    uint8_t token_bytes[MICHI_SESSION_TOKEN_RANDOM_BYTES];
    esp_fill_random(id_bytes, sizeof(id_bytes));
    esp_fill_random(token_bytes, sizeof(token_bytes));
    memset(&s_session, 0, sizeof(s_session));
    memcpy(s_session.info.session_id, MICHI_SESSION_ID_PREFIX,
           strlen(MICHI_SESSION_ID_PREFIX));
    hex_encode(id_bytes, sizeof(id_bytes),
               s_session.info.session_id + strlen(MICHI_SESSION_ID_PREFIX));
    hex_encode(token_bytes, sizeof(token_bytes), s_session.session_token);

    snprintf(s_session.info.owner_controller_id,
             sizeof(s_session.info.owner_controller_id), "%s",
             owner_controller_id);
    strlcpy(s_session.info.codec, codec, sizeof(s_session.info.codec));
    s_session.info.sample_rate = sample_rate;
    s_session.info.bit_depth = bit_depth;
    s_session.info.channels = channels;
    s_session.info.stream_port = stream_port;
    s_session.info.buffer_ms = effective_ms;
    s_session.info.volume = applied_volume;
    s_session.info.paused = false;

    /* Register the engine SSRC (best-effort: with ssrc_filter=0 the
     * source registers on the FIRST ACCEPTED packet, so this is usually
     * NOT_FOUND right after start; the info getter refreshes live). */
    if (michi_audio_session_get_ssrc(&s_session.info.ssrc) != ESP_OK) {
        s_session.info.ssrc = 0;
    }

    s_active = true;
    s_session_start_us = esp_timer_get_time();
    xSemaphoreGive(s_mutex);

    memcpy(out_token, s_session.session_token, MICHI_SESSION_TOKEN_LEN);
    ESP_LOGI(TAG, "session: started id=%s owner=%s port=%u buffer=%u ms "
                  "volume=%u",
             s_session.info.session_id, s_session.info.owner_controller_id,
             (unsigned)s_session.info.stream_port,
             (unsigned)s_session.info.buffer_ms,
             (unsigned)s_session.info.volume);
    post_started_chain();
    return ESP_OK;
}

esp_err_t michi_session_stop(const char *session_token)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!hex_string_ok(session_token, MICHI_SESSION_TOKEN_HEX_LEN)) {
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

    /* Cooperative engine stop: may block up to the join window. A PAUSED
     * session already stopped the engine (stop is idempotent there).
     * ESP_ERR_TIMEOUT: the task did not join - nothing is cleared, retry. */
    const esp_err_t err = michi_audio_session_stop();
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "stop: engine did not stop: %s", esp_err_to_name(err));
        return err;
    }

    s_active = false;
    memset(&s_session, 0, sizeof(s_session));
    s_session_start_us = 0;
    xSemaphoreGive(s_mutex);

    /* F9 follow-up: the IDLE screen must never show stale track info. */
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
    memset(&s_session, 0, sizeof(s_session));
    s_session_start_us = 0;
    xSemaphoreGive(s_mutex);

    if (michi_display_clear_now_playing() != ESP_OK) {
        ESP_LOGW(TAG, "abort: display clear failed (metadata kept)");
    }

    ESP_LOGI(TAG, "session: aborted reason=%s", reason != NULL ? reason : "-");
    post_event(MICHI_EVENT_SESSION_CLOSED);
    return ESP_OK;
}

esp_err_t michi_session_patch(const char *session_token, int volume,
                              bool *paused)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!hex_string_ok(session_token, MICHI_SESSION_TOKEN_HEX_LEN)) {
        return ESP_ERR_INVALID_ARG;
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

    esp_err_t err = ESP_OK;

    if (volume != -1) {
        int v = volume;
        if (v < 0) {
            v = 0;
        } else if (v > 100) {
            v = 100;
        }
        michi_volume_set((uint8_t)v);
        s_session.info.volume = michi_volume_get(); /* applied value (P0-12) */
    }

    if (paused != NULL && *paused != s_session.info.paused) {
        if (*paused) {
            /* Pause: stop the ENGINE, keep the session state. The DAC
             * clocks stay running (continuous silence). */
            err = michi_audio_session_stop();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "patch: pause: engine did not stop: %s",
                         esp_err_to_name(err));
                xSemaphoreGive(s_mutex);
                return err;
            }
            s_session.info.paused = true;
        } else {
            /* Resume (F4): refresh the engine SSRC LIVE and pass it as
             * the filter. The engine registration dies with the engine
             * at pause (get_ssrc is NOT_FOUND while stopped), so this is
             * usually 0 here - first-seen acceptance again; the stored
             * value is informational, never silently reused as filter. */
            uint32_t ssrc_filter = 0;
            if (michi_audio_session_get_ssrc(&ssrc_filter) != ESP_OK) {
                ssrc_filter = 0;
            }
            err = michi_audio_session_start(s_session.info.stream_port,
                                            ssrc_filter);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "patch: resume: engine rejected: %s",
                         esp_err_to_name(err));
                xSemaphoreGive(s_mutex);
                return err;
            }
            s_session.info.paused = false;
        }
    }

    xSemaphoreGive(s_mutex);

    if (volume != -1) {
        ESP_LOGI(TAG, "session: volume=%u (applied)",
                 (unsigned)s_session.info.volume);
    }
    if (paused != NULL && *paused != s_session.info.paused) {
        /* Re-evaluate AFTER the engine call: the flag changed only when
         * the engine accepted. */
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
        /* The engine died on its own (F2): the zombie was cleaned above -
         * report the closure as "no session" to the caller. */
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    *out = s_session.info;
    xSemaphoreGive(s_mutex);

    /* Reconcile the FSM chain (F8): an active session must land the FSM
     * on PLAYING/PAUSED; re-post the missing steps from the current
     * state (from-keyed, idempotent). */
    session_reconcile_fsm();

    /* Live source refresh (best-effort): the engine registers the SSRC
     * and peer on the first accepted packet; until then the stored 0/""
     * is honest. */
    uint32_t ssrc = 0;
    char peer[MICHI_SESSION_SOURCE_ADDR_LEN] = "";
    if (michi_audio_session_get_ssrc(&ssrc) == ESP_OK) {
        out->ssrc = ssrc;
    }
    if (michi_audio_session_get_peer(peer, sizeof(peer)) == ESP_OK) {
        strlcpy(out->source_addr, peer, sizeof(out->source_addr));
    }
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
