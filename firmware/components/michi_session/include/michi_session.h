#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Session lifecycle (phase 12): the SINGLE active audio session
 *        the receiver can hold.
 *
 * The session layer sits between the HTTP API (michi_http, phase 12) and
 * the RTP engine (michi_audio, phase 11). It owns the session contract:
 *
 * - One session at a time. michi_session_start() fails with
 *   ESP_ERR_INVALID_STATE while one is active; a new session must stop
 *   the old one first.
 * - Format validation against the product profile / engine meta 1
 *   (pcm_s16le / 48000 / 16 / 2). Anything else is REJECTED with
 *   ESP_ERR_NOT_SUPPORTED (explicit log naming what was requested) -
 *   never silently remapped.
 * - The session credential: michi_session_start() issues a random 64-hex
 *   session token (esp_fill_random, 32 bytes) that must be presented
 *   (constant-time, single-slot) by stop()/patch(). It is returned to
 *   the controller ONCE by the API layer at creation; the session layer
 *   never logs it. session_id ("sess_" + 16 hex) identifies the session
 *   in heartbeats/diagnostics and is NOT a credential.
 * - The FSM lifecycle is driven by MICHI_EVENT_SESSION_STARTED/CLOSED/
 *   PAUSED/RESUMED (mappings in michi_state.c, phase 12): start posts
 *   SESSION_STARTED once per chain step (IDLE -> SESSION_PENDING ->
 *   BUFFERING -> PLAYING - BUFFERING is modeled retrospectively, the
 *   engine's real prefill is internal to michi_audio), pause posts
 *   SESSION_PAUSED (PLAYING -> PAUSED) and also stops the ENGINE
 *   (michi_audio_session_stop: silence keeps the DAC clocks running,
 *   state retained); resume restarts the engine and posts
 *   SESSION_RESUMED (PAUSED -> PLAYING); stop posts SESSION_CLOSED from
 *   any session state back to IDLE. Event posts are best-effort (warn on
 *   failure): the session layer is the source of truth for the API, the
 *   FSM follows as far as the bus allows.
 * - Pause/resume note: the ENGINE does not hold stream state across
 *   pause - the jitter buffer is torn down AND the source registration
 *   (SSRC/peer) dies with it. Resume refreshes the SSRC live via
 *   michi_audio_session_get_ssrc() and passes it as the filter when a
 *   live registration exists; with the engine stopped at pause there is
 *   usually none, so the resumed session falls back to first-seen
 *   acceptance (filter 0) - the stored SSRC is informational only.
 * - Dead-engine reconciliation: the engine task can self-terminate
 *   (socket/bind failure or a pipeline write rejection). get_info() and
 *   start() detect an active session whose engine died
 *   (michi_audio_session_active() false, outside the post-start grace
 *   window and not paused) and clean it up: state cleared,
 *   MICHI_EVENT_SESSION_CLOSED posted, "cleaned dead engine session"
 *   logged. start() then proceeds with the new session; get_info()
 *   returns ESP_ERR_INVALID_STATE (the API answers "no active session").
 * - OTA gate (phase 13): start() rejects with ESP_ERR_INVALID_STATE while
 *   the FSM is in MICHI_STATE_UPDATING (the HTTP layer answers 409
 *   ota_in_progress BEFORE the session layer is reached - the gate here
 *   is defensive). The update path force-closes the session with
 *   michi_session_abort() (privileged, no credential - see below).
 * - FSM reconciliation: the FSM follows the session layer best-effort.
 *   get_info() re-posts the missing SESSION_STARTED chain steps from
 *   the current state when an active session left the FSM short of
 *   PLAYING/PAUSED (from-keyed posts: idempotent, self-healing).
 * - No persistence: sessions live in RAM only (NVS untouched). A reboot
 *   ends the session; the controller re-pairs/restarts it.
 *
 * Threading: all calls are task-context (the httpd task); a mutex
 * serializes access. stop() - and pause() inside patch(), which stops
 * the engine the same way - may block up to the engine's cooperative
 * join window (~2 s, MICHI_AUDIO_JOIN_TIMEOUT_MS) while the session
 * task winds down; they run on the httpd task by design (single
 * session, single server task).
 */

/*!< session_id: "sess_" + 8 random bytes hex + NUL (24 keeps slack) */
#define MICHI_SESSION_ID_LEN 24
#define MICHI_SESSION_ID_PREFIX "sess_"
/*!< session token: 32 random bytes = 64 hex chars + NUL (never logged,
 * never persisted; returned once at creation). The spec sketch said 64
 * for the hex string; the buffer MUST hold the NUL too. */
#define MICHI_SESSION_TOKEN_HEX_LEN 64
#define MICHI_SESSION_TOKEN_LEN (MICHI_SESSION_TOKEN_HEX_LEN + 1)
/*!< owner controller id (same charset rule as the pairing registry) */
#define MICHI_SESSION_OWNER_MAX 31
/*!< source address buffer (longest IPv4 dotted string fits in 16) */
#define MICHI_SESSION_SOURCE_ADDR_LEN 48
/*!< codec string ("pcm_s16le" in meta 1) */
#define MICHI_SESSION_CODEC_LEN 16

/**
 * @brief Snapshot of the active session (michi_session_get_info).
 *
 * `volume` is the APPLIED value (michi_volume_get after set - P0-12);
 * `buffer_ms` is the value actually in effect (clamped to the engine's
 * jitter capacity); `ssrc`/`source_addr` reflect the source registered
 * by the engine (0 / empty until the first accepted packet).
 */
typedef struct {
    char session_id[MICHI_SESSION_ID_LEN];        /* "sess_" + 16 hex */
    char owner_controller_id[32];                 /* pairing controller id */
    uint32_t ssrc;                                /* source SSRC (0 = not yet registered) */
    char source_addr[MICHI_SESSION_SOURCE_ADDR_LEN]; /* dotted IPv4 ("" = not yet registered) */
    char codec[MICHI_SESSION_CODEC_LEN];          /* "pcm_s16le" (meta 1) */
    uint32_t sample_rate;                         /* 48000 (validated baseline) */
    uint8_t bit_depth;                            /* 16 */
    uint8_t channels;                             /* 2 */
    uint16_t stream_port;                         /* UDP port bound by the engine */
    uint16_t buffer_ms;                           /* jitter budget in effect (clamped) */
    uint8_t volume;                               /* APPLIED volume 0-100 */
    bool paused;                                  /* true while the engine is stopped on purpose */
} michi_session_info_t;

/**
 * @brief Initialize the session layer.
 *
 * Must be called after michi_audio_init() (the layer calls into the
 * engine at session start/stop). No NVS access. Safe to call once;
 * repeated calls return ESP_OK (idempotent). On failure app_main
 * continues degraded: no sessions, everything else keeps working.
 *
 * @return ESP_OK; ESP_ERR_NO_MEM if the mutex cannot be created.
 */
esp_err_t michi_session_init(void);

/**
 * @brief Start the single active session.
 *
 * Validation order: initialized (INVALID_STATE) -> no session active
 * (INVALID_STATE) -> owner id format 1..31 chars alphanumeric + '-'
 * (INVALID_ARG) -> codec in the product profile supported_codecs and
 * exactly "pcm_s16le" (NOT_SUPPORTED, explicit log; "pcm_s24le" is
 * rejected as a declared meta) -> format via the engine output ops
 * prepare(): 48000/16/2 only (NOT_SUPPORTED) -> stream_port in
 * [1024, 65535] (INVALID_ARG, the API has no ephemeral-port mode).
 * buffer_ms is CLAMPED to [50, CONFIG_MICHI_AUDIO_JITTER_MAX_MS] (the
 * engine's jitter capacity) and the clamped value is stored + logged;
 * volume is clamped to 0-100 via michi_volume_set() and the APPLIED
 * value is stored.
 *
 * Then: michi_audio_session_start(stream_port, ssrc_filter=0) -> the
 * engine task binds the port (an INVALID_STATE return means the audio
 * pipeline is not running - no DAC); the session id + token are
 * generated with esp_fill_random; the engine SSRC is registered
 * (best-effort: usually unknown until the first accepted packet, so the
 * info field stays 0 until then); MICHI_EVENT_SESSION_STARTED is posted
 * three times, driving IDLE -> SESSION_PENDING -> BUFFERING -> PLAYING
 * (best-effort, warn on failure).
 *
 * A session whose ENGINE self-terminated (dead-engine reconciliation,
 * see the top note) is cleaned BEFORE the active check: the zombie state
 * is cleared (SESSION_CLOSED posted, "session: cleaned dead engine
 * session" logged) and the new start proceeds - a live session still
 * answers ESP_ERR_INVALID_STATE.
 *
 * The session token is returned in `out_token` (64 hex chars) - the
 * ONLY time the caller receives it.
 *
 * @param owner_controller_id The authenticated controller id (1..31
 *                            chars; the API layer passes the Bearer
 *                            token owner, never a body claim).
 * @param codec               "pcm_s16le" (meta 1).
 * @param sample_rate         48000.
 * @param bit_depth           16.
 * @param channels            2.
 * @param stream_port         UDP port 1024..65535.
 * @param buffer_ms           Requested jitter budget; clamped to the
 *                            engine capacity (>= 50).
 * @param volume              Requested volume; clamped to 0-100.
 * @param out_token           Buffer for the session token
 *                            (>= MICHI_SESSION_TOKEN_LEN).
 * @param out_token_len       Size of out_token.
 * @return ESP_OK; ESP_ERR_INVALID_STATE (before init, session already
 *         active, or the audio pipeline is not running);
 *         ESP_ERR_INVALID_ARG (bad owner id, port out of range,
 *         too-small token buffer); ESP_ERR_NOT_SUPPORTED (codec or
 *         format outside meta 1 - rejected explicitly);
 *         ESP_ERR_NO_MEM (engine buffers/task); engine errors
 *         propagate unchanged.
 */
esp_err_t michi_session_start(const char *owner_controller_id,
                              const char *codec,
                              uint32_t sample_rate, uint8_t bit_depth,
                              uint8_t channels, uint16_t stream_port,
                              uint16_t buffer_ms, uint8_t volume,
                              char *out_token, size_t out_token_len);

/**
 * @brief Stop the active session (engine stop + MICHI_EVENT_SESSION_CLOSED).
 *
 * The session token is validated in CONSTANT TIME against the active
 * session (fixed 64-char comparison; malformed tokens are rejected as
 * INVALID_ARG without comparison - no timing side channel, the token is
 * a credential). Engine stop is cooperative: this call may block up to
 * the join window. Idempotent for an already-stopped engine (pause)
 * since michi_audio_session_stop() is; the session state is cleared in
 * every case. If the engine task fails to join, ESP_ERR_TIMEOUT is
 * returned and NOTHING is cleared - retry stop(). On success the
 * now-playing display metadata is cleared (F9 follow-up) so the IDLE
 * screen never shows stale track info.
 *
 * @param session_token The 64-hex session token issued at start.
 * @return ESP_OK; ESP_ERR_INVALID_STATE (before init, or no session
 *         active); ESP_ERR_INVALID_ARG (malformed token); ESP_ERR_NOT_FOUND
 *         (token does not match the active session);
 *         ESP_ERR_TIMEOUT (engine task did not join - retry).
 */
esp_err_t michi_session_stop(const char *session_token);

/**
 * @brief Force-stop the active session WITHOUT the credential
 *        (privileged internal path, phase 13 OTA).
 *
 * The session token is a credential that is never persisted, so the OTA
 * component cannot present it when it must tear the session down before
 * flashing. This API is for trusted firmware components only (michi_ota);
 * it must never be reachable from the network layer (the HTTP handlers
 * use stop()/patch() exclusively).
 *
 * Same semantics as stop() minus the credential check: engine stop
 * (cooperative, may block up to the join window), session state cleared,
 * MICHI_EVENT_SESSION_CLOSED posted, now-playing metadata cleared.
 *
 * @param reason Log reason (e.g. "ota update"); may be NULL.
 * @return Same as stop() (INVALID_STATE when no session is active;
 *         TIMEOUT when the engine did not join - retry).
 */
esp_err_t michi_session_abort(const char *reason);

/**
 * @brief Patch the active session: volume and/or pause state.
 *
 * volume == -1: no change. Any other value is clamped to 0-100 and
 * applied via michi_volume_set() (the info volume is refreshed with the
 * APPLIED value). paused == NULL: no change. paused == true while
 * playing: engine stop (state retained) + MICHI_EVENT_SESSION_PAUSED
 * (PLAYING -> PAUSED) - the engine stop BLOCKS up to the ~2 s join
 * window (same as stop(), see the threading note); paused == false
 * while paused: engine restart on the session port (ssrc_filter = a
 * LIVE engine SSRC if one is registered, else 0 - the registration dies
 * with the engine at pause, see the top note) +
 * MICHI_EVENT_SESSION_RESUMED (PAUSED -> PLAYING). Pausing while the FSM
 * is outside PLAYING (PENDING/BUFFERING) still stops the engine; the
 * PAUSED event is broadcast-only there (no mapping) - documented FSM
 * divergence, the session info is authoritative.
 *
 * @param session_token The 64-hex session token.
 * @param volume        -1 = no change; else clamped 0-100 and applied.
 * @param paused        NULL = no change; else the target pause state.
 * @return ESP_OK; ESP_ERR_INVALID_STATE (before init, no session
 *         active); ESP_ERR_INVALID_ARG (malformed token);
 *         ESP_ERR_NOT_FOUND (token mismatch); engine start/stop errors
 *         propagate unchanged (e.g. INVALID_STATE when the pipeline is
 *         down on resume - nothing was patched beyond what succeeded).
 */
esp_err_t michi_session_patch(const char *session_token, int volume,
                              bool *paused);

/**
 * @brief Copy the active session snapshot (token never included).
 *
 * Reconciles reality first: a session whose engine self-terminated is
 * cleaned here (SESSION_CLOSED posted, "cleaned dead engine session"
 * logged) and ESP_ERR_INVALID_STATE is returned (the API answers "no
 * active session"); a stuck FSM (active session outside PLAYING/PAUSED)
 * is re-driven by re-posting the missing SESSION_STARTED steps from the
 * current state (from-keyed, idempotent - see the top note). Then:
 * `ssrc`/`source_addr` are refreshed LIVE from the engine (best-effort:
 * they stay 0/"" until the first accepted packet) - the session layer
 * stored copy registers the start-time value.
 *
 * @param out Output struct (must not be NULL).
 * @return ESP_OK; ESP_ERR_INVALID_STATE (before init, no session
 *         active, or a dead-engine session was just cleaned);
 *         ESP_ERR_INVALID_ARG on NULL out.
 */
esp_err_t michi_session_get_info(michi_session_info_t *out);

/**
 * @return true while a session is active (started, not yet stopped).
 *         A PAUSED session is still active.
 */
bool michi_session_active(void);

/**
 * @return true once michi_session_init() succeeded. Lets the API layer
 *         distinguish "session layer unavailable" (init failed at boot)
 *         from "audio unavailable" (no DAC) when start() fails.
 */
bool michi_session_is_initialized(void);

#ifdef __cplusplus
}
#endif
