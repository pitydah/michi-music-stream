#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Session lifecycle (MS-07): the SINGLE canonical audio session
 *        the receiver can hold (contract section 2.5).
 *
 * The session layer sits between the HTTP API (michi_http) and the RTP
 * engine (michi_audio). It owns the session contract:
 *
 * - One session at a time. michi_session_start() fails with
 *   ESP_ERR_INVALID_STATE while one is active.
 * - STRICT validation, no clamping: every negotiated field must be
 *   exactly canonical (transport rtp_udp / codec pcm_s16le / 48000 / 16
 *   / 2 / packet_ms 10 / buffer_ms 50..500 / payload_type 97 / ssrc
 *   1..4294967295 / volume 0..100). Invalid values are REJECTED - never
 *   rounded, remapped or clamped (the HTTP layer answers 400
 *   INVALID_REQUEST with details.field BEFORE this layer is reached;
 *   the checks here are defensive).
 * - The session credential: michi_session_start() issues a 32-byte
 *   CSPRNG session token, encoded base64url WITHOUT padding (43 chars),
 *   that must be presented (constant-time) by stop()/patch(). It is
 *   returned to the controller ONCE by the API layer at creation,
 *   exists ONLY in RAM, is never logged and never persisted; a stop
 *   wipes it from RAM. session_id is a UUID v4 identifying the session
 *   in heartbeats/diagnostics and is NOT a credential.
 * - The receiver picks the UDP stream port: michi_audio binds a free
 *   port in 49152..65535 (never taken from the request). The RTP source
 *   IP is the HTTP request peer, captured by the API layer and passed
 *   here - it can never arrive in JSON.
 * - All-or-nothing start: idle -> starting -> engine start (socket +
 *   buffers + task, synchronous bind) -> playing. On ANY engine failure
 *   (bind/buffer/pipeline/task) the engine has already released
 *   everything it reserved and the session layer rolls back to idle: a
 *   phantom session cannot exist.
 * - Pause is a session state, not a teardown: the engine keeps its
 *   socket/task alive (valid packets keep being counted and are
 *   discarded - silence). Delete/stop does the full teardown: engine
 *   stop, silence, buffers/socket freed, token wiped from RAM.
 * - The FSM lifecycle is driven by MICHI_EVENT_SESSION_STARTED/CLOSED/
 *   PAUSED/RESUMED (best-effort posts; the session layer is the source
 *   of truth for the API).
 * - Dead-engine reconciliation: if the engine self-terminates (pipeline
 *   write rejection) while a session is active, get_info()/start() clean
 *   the zombie state and report "no session".
 * - OTA gate: start() rejects with ESP_ERR_INVALID_STATE while the FSM
 *   is in MICHI_STATE_UPDATING (the HTTP layer answers 409 BEFORE this
 *   layer is reached - the gate here is defensive). The update path
 *   force-closes the session with michi_session_abort() (privileged, no
 *   credential).
 * - No persistence: sessions live in RAM only (NVS untouched). A reboot
 *   ends the session; the controller re-pairs/restarts it.
 * - The lease (MS-08, contract section 2.6): start and every valid
 *   heartbeat renew a 30 s lease on the MONOTONIC clock (esp_timer).
 *   The watchdog is a one-shot esp_timer armed ONLY while a session is
 *   active; when it fires (30 s without renewal, RTP traffic does NOT
 *   count) it runs the SAME safe teardown as DELETE, increments
 *   lease_expirations and returns the layer to idle. API entry points
 *   reconcile the deadline lazily too (mirroring the simulator
 *   reference), so GET/PATCH/DELETE/heartbeat on an expired session
 *   answer 404 and a new start proceeds - even if the timer task is
 *   delayed. GET reports the REAL remaining window
 *   (lease_remaining_ms = deadline - monotonic now, floor).
 *
 * Threading: all calls are task-context (the httpd task) except the
 * watchdog callback (the esp_timer task); a mutex serializes access.
 * stop() may block up to the engine's cooperative join window (~2 s,
 * MICHI_AUDIO_JOIN_TIMEOUT_MS) while the session task winds down; it
 * runs on the httpd task by design (single session, single server
 * task). The watchdog callback performs the same engine join - the
 * esp_timer task is blocked at most that long once per lease expiry.
 */

/*!< session_id: UUID v4 (8-4-4-4-12 lowercase hex) + NUL */
#define MICHI_SESSION_ID_LEN 37
/*!< session token: 32 random bytes = 43 base64url-nopad chars + NUL
 * (never logged, never persisted, RAM only; returned once at creation) */
#define MICHI_SESSION_TOKEN_B64_LEN 43
#define MICHI_SESSION_TOKEN_LEN (MICHI_SESSION_TOKEN_B64_LEN + 1)
/*!< owner controller id (same charset rule as the pairing registry) */
#define MICHI_SESSION_OWNER_MAX 31
/*!< source address buffer (longest IPv4 dotted string fits in 16) */
#define MICHI_SESSION_SOURCE_ADDR_LEN 16
/*!< codec string ("pcm_s16le") */
#define MICHI_SESSION_CODEC_LEN 16
/*!< lease window (contract section 2.5/2.6; renewal = MS-08) */
#define MICHI_SESSION_LEASE_SECONDS 30
#define MICHI_SESSION_LEASE_MS (MICHI_SESSION_LEASE_SECONDS * 1000)

/**
 * @brief Internal session lifecycle state (contract state machine:
 *        idle -> starting -> playing -> paused -> stopping -> idle).
 */
typedef enum {
    MICHI_SESSION_STATE_STARTING = 0, /*!< Resources being reserved */
    MICHI_SESSION_STATE_PLAYING,      /*!< Engine running */
    MICHI_SESSION_STATE_PAUSED,       /*!< Engine alive, audio suspended */
    MICHI_SESSION_STATE_STOPPING,     /*!< Teardown in progress */
} michi_session_state_t;

/**
 * @brief Result of michi_session_heartbeat() (the HTTP layer maps it:
 *        TOKEN_MISMATCH -> 401, NO_SESSION/SESSION_MISMATCH -> 404,
 *        SEQUENCE_REPLAY -> 409 CONFLICT without renewing).
 */
typedef enum {
    MICHI_SESSION_HEARTBEAT_OK = 0,      /*!< Lease renewed to 30 s */
    MICHI_SESSION_HEARTBEAT_NO_SESSION,  /*!< No session (or lease expired first) */
    MICHI_SESSION_HEARTBEAT_TOKEN_MISMATCH, /*!< Wrong/malformed session token */
    MICHI_SESSION_HEARTBEAT_SESSION_MISMATCH, /*!< session_id != active session */
    MICHI_SESSION_HEARTBEAT_SEQUENCE_REPLAY,  /*!< sequence repeated/older: no renew */
} michi_session_heartbeat_result_t;

/**
 * @brief Snapshot of the active session (michi_session_get_info).
 *
 * `volume` is the APPLIED value (michi_volume_get after set - the
 * reported value is always the value actually applied); `ssrc` is the
 * NEGOTIATED SSRC (never first-seen); `source_addr` is the HTTP request
 * peer; the packet counters are the engine metrics (packets_rejected =
 * the sum of the per-class datagram rejects).
 */
typedef struct {
    char session_id[MICHI_SESSION_ID_LEN]; /* UUID v4 */
    char owner_controller_id[32];          /* pairing controller id */
    michi_session_state_t state;           /* starting/playing/paused/stopping */
    char codec[MICHI_SESSION_CODEC_LEN];   /* "pcm_s16le" */
    uint32_t sample_rate;                  /* 48000 */
    uint8_t bit_depth;                     /* 16 */
    uint8_t channels;                      /* 2 */
    uint8_t packet_ms;                     /* 10 */
    uint8_t payload_type;                  /* 97 */
    uint32_t ssrc;                         /* negotiated (1..4294967295) */
    char source_addr[MICHI_SESSION_SOURCE_ADDR_LEN]; /* dotted IPv4 peer */
    uint16_t stream_port;                  /* UDP port bound by the engine */
    uint16_t buffer_ms;                    /* jitter budget in effect (50..500) */
    uint8_t volume;                        /* APPLIED volume 0-100 */
    bool paused;                           /* true while playback is suspended */
    uint32_t lease_remaining_ms;           /* REAL remaining window (monotonic
                                             deadline - now, floor, clamped
                                             >= 0; MS-08) */
    uint32_t packets_received;             /* accepted past the guard */
    uint32_t packets_rejected;             /* source/PT/SSRC/size rejects */
    uint32_t packets_lost;                 /* detected seq gaps */
    uint32_t underruns;                    /* jitter-buffer-empty events */
} michi_session_info_t;

/**
 * @brief Negotiated session parameters (POST /receiver-lite/session,
 *        already validated by the HTTP body gate - the fields here are
 *        re-validated defensively, never clamped).
 */
typedef struct {
    const char *owner_controller_id; /*!< Authenticated controller id (the
                                          Bearer token owner, never a body
                                          claim). */
    const char *codec;               /*!< "pcm_s16le" */
    uint32_t sample_rate;            /*!< 48000 */
    uint8_t bit_depth;               /*!< 16 */
    uint8_t channels;                /*!< 2 */
    uint8_t packet_ms;               /*!< 10 */
    uint16_t buffer_ms;              /*!< 50..500 */
    uint8_t payload_type;            /*!< 97 */
    uint32_t ssrc;                   /*!< 1..4294967295 */
    uint8_t volume;                  /*!< 0..100 */
    const char *source_ip;           /*!< Dotted IPv4 of the HTTP request
                                          peer (the only accepted RTP
                                          source). Never from JSON. */
} michi_session_start_params_t;

/**
 * @brief Initialize the session layer.
 *
 * Must be called after michi_audio_init() (the layer calls into the
 * engine at session start/stop). No NVS access. Safe to call once;
 * repeated calls return ESP_OK (idempotent). Creates the lease
 * watchdog one-shot esp_timer. On failure app_main continues degraded:
 * no sessions, everything else keeps working (a session without a
 * watchdog could never expire - the contract demands the watchdog, so
 * init fails instead of allowing lease-less sessions).
 *
 * @return ESP_OK; ESP_ERR_NO_MEM if the mutex or the watchdog timer
 *         cannot be created.
 */
esp_err_t michi_session_init(void);

/**
 * @brief Whether a string is a well-formed session token (exactly 43
 *        base64url chars without padding). Used by the HTTP layer for
 *        the 401 fast path and internally by stop()/patch().
 *
 * @param token Token to check (NULL = false).
 * @return true when the token has the canonical shape.
 */
bool michi_session_token_valid(const char *token);

/**
 * @brief Start the single canonical session (idle -> starting -> playing).
 *
 * Validation order: initialized (INVALID_STATE) -> output buffers
 * (INVALID_ARG) -> strict negotiation (INVALID_ARG/INVALID_STATE, no
 * clamping) -> no session active (INVALID_STATE) -> engine start
 * (socket + buffers + task, synchronous bind; any failure releases
 * everything and returns the error - the session layer stays idle).
 * On success the receiver-picked stream port is read back from the
 * engine, the volume is applied (the APPLIED value is stored), the
 * UUID v4 session id and the 32-byte base64url-nopad session token are
 * generated with esp_fill_random and MICHI_EVENT_SESSION_STARTED is
 * posted three times, driving IDLE -> SESSION_PENDING -> BUFFERING ->
 * PLAYING (best-effort).
 *
 * A session whose ENGINE self-terminated (dead-engine reconciliation)
 * is cleaned BEFORE the active check: the zombie state is cleared
 * (SESSION_CLOSED posted, "cleaned dead engine session" logged) and the
 * new start proceeds - a live session still answers ESP_ERR_INVALID_STATE.
 * A session whose LEASE expired is also reconciled first (the same
 * teardown as DELETE, lease_expirations incremented): mirroring the
 * simulator reference, an expired session is released and the new start
 * proceeds instead of answering 409.
 *
 * On success the 30 s lease starts on the MONOTONIC clock (esp_timer)
 * and the watchdog is armed.
 *
 * The session token is returned in `out_token` (43 base64url chars) -
 * the ONLY time the caller receives it.
 *
 * @param params        Negotiated parameters (all fields required).
 * @param out_token     Buffer for the session token
 *                      (>= MICHI_SESSION_TOKEN_LEN).
 * @param out_token_len Size of out_token.
 * @return ESP_OK; ESP_ERR_INVALID_STATE (before init, session already
 *         active, FSM UPDATING, or the audio pipeline is not running);
 *         ESP_ERR_INVALID_ARG (bad owner id, non-canonical format,
 *         buffer_ms/ssrc/volume out of range, bad source IP, too-small
 *         token buffer); ESP_ERR_NOT_SUPPORTED (codec outside the
 *         canonical profile - rejected explicitly); ESP_ERR_NO_MEM /
 *         ESP_FAIL (engine bind/buffer/task failure - everything was
 *         released, no session exists).
 */
esp_err_t michi_session_start(const michi_session_start_params_t *params,
                              char *out_token, size_t out_token_len);

/**
 * @brief Stop the active session (engine teardown + MICHI_EVENT_SESSION_CLOSED).
 *
 * The session token is validated in CONSTANT TIME against the active
 * session (fixed 43-char comparison; malformed tokens are rejected as
 * INVALID_ARG without comparison - no timing side channel). The lease
 * deadline is reconciled FIRST: an expired session is already closed
 * (lease_expirations incremented) and stop() reports INVALID_STATE
 * (HTTP 404), mirroring the simulator reference. Engine stop is
 * cooperative: this call may block up to the join window. On success
 * the engine socket/buffers are freed, the session state is cleared and
 * the token is WIPED FROM RAM (memset) - GET then answers 404.
 *
 * @param session_token The 43-char base64url session token issued at start.
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
 * The session token is a RAM-only credential, so the OTA component
 * cannot present it when it must tear the session down before flashing.
 * This API is for trusted firmware components only (michi_ota); it must
 * never be reachable from the network layer (the HTTP handlers use
 * stop()/patch() exclusively).
 *
 * Same semantics as stop() minus the credential check: engine stop
 * (cooperative, may block up to the join window), session state cleared,
 * token wiped, MICHI_EVENT_SESSION_CLOSED posted.
 *
 * @param reason Log reason (e.g. "ota update"); may be NULL.
 * @return Same as stop() (INVALID_STATE when no session is active;
 *         TIMEOUT when the engine did not join - retry).
 */
esp_err_t michi_session_abort(const char *reason);

/**
 * @brief Patch the active session: volume and/or pause state.
 *
 * volume_set: apply 0..100 via michi_volume_set() (the info volume is
 * refreshed with the APPLIED value). paused_set: transition the session
 * between playing and paused (the engine keeps its socket/task alive -
 * valid packets are counted and discarded while paused) +
 * MICHI_EVENT_SESSION_PAUSED/RESUMED.
 *
 * @param session_token The 43-char base64url session token.
 * @param volume_set    true to apply `volume`.
 * @param volume        New volume 0..100 (ignored when !volume_set).
 * @param paused_set    true to apply `paused`.
 * @param paused        Target pause state (ignored when !paused_set).
 * @return ESP_OK; ESP_ERR_INVALID_STATE (before init, no session
 *         active); ESP_ERR_INVALID_ARG (malformed token, volume > 100);
 *         ESP_ERR_NOT_FOUND (token mismatch).
 */
esp_err_t michi_session_patch(const char *session_token, bool volume_set,
                              uint8_t volume, bool paused_set, bool paused);

/**
 * @brief Process a session heartbeat (contract section 2.6, MS-08).
 *
 * Guard order mirrors the simulator reference: the lease deadline is
 * reconciled FIRST (an expired session is closed with the same teardown
 * as DELETE and lease_expirations is incremented - the heartbeat then
 * finds no session), then the credential, then the session_id, then the
 * sequence. `sent_at_ms` is NOT an input: it is informational per the
 * contract and never used for the local timeout.
 *
 * `sequence` is unsigned and STRICTLY increasing within the session
 * (the first heartbeat of a session may be any value). A valid
 * heartbeat renews the lease to 30 s on the MONOTONIC clock (esp_timer)
 * and re-arms the watchdog; a repeated or older sequence does NOT renew
 * and reports SEQUENCE_REPLAY (HTTP 409 CONFLICT).
 *
 * @param session_token The 43-char base64url session token.
 * @param session_id    UUID v4 of the active session (from the body).
 * @param sequence      Unsigned heartbeat sequence (strictly increasing).
 * @return MICHI_SESSION_HEARTBEAT_OK on renewal; NO_SESSION (404);
 *         TOKEN_MISMATCH (401); SESSION_MISMATCH (404);
 *         SEQUENCE_REPLAY (409, no renewal).
 */
michi_session_heartbeat_result_t michi_session_heartbeat(
    const char *session_token, const char *session_id, uint32_t sequence);

/**
 * @brief Cumulative lease-expiry close count (contract metric, MS-08).
 *
 * Incremented every time the lease watchdog or the lazy reconciliation
 * closes a session because the 30 s window expired (never on DELETE or
 * abort). Survives session closes; reset only by reboot. Exposed by the
 * diagnostics session block.
 *
 * @return The counter (0 before init).
 */
uint32_t michi_session_lease_expirations(void);

/**
 * @brief Copy the active session snapshot (token never included).
 *
 * Reconciles reality first: a session whose LEASE expired is closed
 * here (the same teardown as DELETE, lease_expirations incremented) and
 * ESP_ERR_INVALID_STATE is returned (the API answers "no active
 * session"); a session whose engine self-terminated is cleaned here
 * (SESSION_CLOSED posted, "cleaned dead engine session" logged) and
 * ESP_ERR_INVALID_STATE is returned; a stuck FSM is re-driven by
 * re-posting the missing SESSION_STARTED steps (from-keyed,
 * idempotent). Then the packet counters are refreshed LIVE from the
 * engine metrics and lease_remaining_ms reports the REAL remaining
 * window on the monotonic clock (deadline - now, floor, clamped >= 0).
 *
 * @param out Output struct (must not be NULL).
 * @return ESP_OK; ESP_ERR_INVALID_STATE (before init, no session
 *         active, or a lease-expired/dead-engine session was just
 *         cleaned); ESP_ERR_INVALID_ARG on NULL out.
 */
esp_err_t michi_session_get_info(michi_session_info_t *out);

/**
 * @return true while a session is active (started, not yet stopped).
 *         A PAUSED session is still active. Reconciles the lease first:
 *         an expired session is closed (same teardown as DELETE,
 *         lease_expirations incremented) and false is returned, so the
 *         HTTP fast paths match the simulator reference (an expired
 *         session answers 404 / allows a new start instead of 409).
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
