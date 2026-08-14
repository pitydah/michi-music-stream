#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "michi_identity.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Receiver-button pairing (MS-06): the canonical Michi Link pairing flow
 * (convergence spec, section 2.3).
 *
 * Contract highlights:
 * - The physical button is the ONLY authority that opens the pairing
 *   window: michi_pairing_open_window() is called exclusively from the
 *   button path (michi_button) or an equivalent physical gate. There is
 *   NO network-visible API that opens it - the HTTP pair handlers only
 *   operate INSIDE a window already opened by the button.
 * - A window lasts exactly CONFIG_MICHI_PAIRING_WINDOW_SECONDS (default
 *   120) on the MONOTONIC clock (esp_timer). A reboot closes it (window
 *   state is RAM-only). Re-opening replaces the previous window AND
 *   drops every pending pairing session.
 * - POST /pair/start verifies an Ed25519 signature over the DECODED
 *   challenge_nonce bytes (michi_identity_verify) and that michi_id ==
 *   base64url-nopad(blake3(public_key)) (michi_identity_derive_michi_id).
 *   Any failure answers 400 and creates NO session.
 * - On success a 6-digit PIN is drawn UNIFORMLY from esp_fill_random
 *   (rejection sampling: no modulo bias), kept in RAM ONLY, shown
 *   locally through the PIN display callback and NEVER returned by
 *   HTTP.
 * - POST /pair/confirm: the controller identity must match pair/start
 *   exactly. Five failed PIN attempts are allowed (401); the SIXTH
 *   confirm attempt answers 429 and the session is consumed (locked).
 *   A correct PIN issues a receiver-generated token: 32 bytes from
 *   esp_fill_random, base64url WITHOUT padding (43 chars), returned
 *   ONCE with expires_in 0 ("no automatic expiry"). Only the SHA-256
 *   digest of the token is persisted - with the controller michi_id,
 *   public_key, permissions, creation date and last activity. A second
 *   (replay) confirmation answers 409.
 * - Token validation (HTTP Bearer) compares digests in CONSTANT TIME
 *   against every slot, without early return (no length/timing oracle).
 * - Rate limits (section MS-06: fixed CONSTANTS, not Kconfig): per
 *   window, MICHI_PAIRING_MAX_STARTS_PER_WINDOW pair/start requests
 *   globally and MICHI_PAIRING_MAX_STARTS_PER_IP per source IP.
 * - Zero secrets in logs: PIN, token, digest, nonce and signature are
 *   NEVER logged. Session ids, controller michi_ids (public) and
 *   counters may appear.
 * - Revocation (michi_pairing_revoke) and factory reset
 *   (michi_pairing_erase_all, or the device-wide NVS erase) clear the
 *   controller registry.
 * - The legacy fields initiator_id and client_token are rejected by
 *   the HTTP body gate (400) - they are not part of this flow.

 * Persistence: the registry lives in NVS as ONE versioned blob (key
 * "controllers", namespace "michi_pairing"). Blob layout
 * (MICHI_PAIRING_BLOB_VERSION = 2; version 1 was the phase-10 challenge
 * registry and is treated as EMPTY on load):
 *   offset 0:  u32 version (MICHI_PAIRING_BLOB_VERSION)
 *   offset 4:  u32 count
 *   offset 8:  count entries (184 bytes each), field order:
 *                device_id[37]        (UUID v4 + NUL, assigned by receiver)
 *                michi_id[44]         (controller michi_id, 43 chars + NUL)
 *                public_key[44]       (controller public key b64url + NUL)
 *                digest[32]           (SHA-256 of the token, never the token)
 *                permissions          (u32 bitmask)
 *                reserved             (u32, layout stability)
 *                created_unix         (int64)
 *                last_activity_unix   (int64)
 *   The layout is _Static_assert-guarded; entries are zeroed before
 *   filling so the persisted bytes are deterministic. Every mutation
 *   rewrites the whole blob (nvs_set_blob + commit) and propagates NVS
 *   errors. Slots [count..MAX) are zeroed before persisting (no revoked
 *   digests retained on flash).
 */

/* Controller permissions (bitmask, uint32_t) - kept from the phase-10
 * design; MS-06 stores the granted bitmask per controller. */
#define MICHI_PERM_STATUS           0x00000001u /*!< Read status/state */
#define MICHI_PERM_PLAYBACK         0x00000002u /*!< Start/stop/pause playback */
#define MICHI_PERM_VOLUME           0x00000004u /*!< Read/set volume */
#define MICHI_PERM_SETTINGS         0x00000008u /*!< Read/change device settings */
#define MICHI_PERM_CONTROLLER_ADMIN 0x00000010u /*!< Manage controllers (revoke, list) */
#define MICHI_PERM_OTA              0x00000020u /*!< Trigger/authorize OTA */
#define MICHI_PERM_FACTORY_RESET    0x00000040u /*!< Trigger factory reset */

/* Permissions granted to a controller when it pairs (default grant). The
 * elevated bits above are intentionally NOT included: they require a
 * future grant-management flow. */
#define MICHI_PERM_DEFAULT (MICHI_PERM_STATUS | MICHI_PERM_PLAYBACK | \
                            MICHI_PERM_VOLUME | MICHI_PERM_SETTINGS)

/* Contract constants (section 2.3 + MS-06 "en constantes"). */
#define MICHI_PAIRING_PIN_ATTEMPTS 5u           /*!< Max failed PIN attempts per session */
#define MICHI_PAIRING_PIN_LEN 6u                /*!< PIN digits */
#define MICHI_PAIRING_PIN_BUF_LEN (MICHI_PAIRING_PIN_LEN + 1u)
#define MICHI_PAIRING_TOKEN_BYTES 32u           /*!< Receiver-issued token size */
#define MICHI_PAIRING_TOKEN_B64_LEN 44u         /*!< 43 base64url chars + NUL */
#define MICHI_PAIRING_DIGEST_BYTES 32u          /*!< SHA-256 */
#define MICHI_PAIRING_MAX_STARTS_PER_WINDOW 5u  /*!< Global pair/start rate limit per window */
#define MICHI_PAIRING_MAX_STARTS_PER_IP 3u      /*!< Per-source-IP pair/start rate limit per window */
#define MICHI_PAIRING_MAX_SESSIONS_PER_WINDOW 4u /*!< Active pairing sessions per window */
#define MICHI_PAIRING_SESSION_ID_LEN 37u        /*!< UUID v4 + NUL */
#define MICHI_PAIRING_DEVICE_ID_LEN 37u         /*!< Controller UUID v4 + NUL */
#define MICHI_PAIRING_EXPIRES_AT_LEN 25u        /*!< RFC 3339 "YYYY-MM-DDTHH:MM:SSZ" + NUL */
#define MICHI_PAIRING_NONCE_B64_MAX 64u         /*!< challenge_nonce buffer (schema: >= 22 chars) */
#define MICHI_PAIRING_IP_MAX 46u                /*!< Source IP string (IPv4/IPv6) + NUL */
#define MICHI_PAIRING_ACTIVITY_PERSIST_SECONDS 60u /*!< Min interval between last-activity NVS writes */

/* The peer (controller) identity carried by pair/start and replayed by
 * pair/confirm. All fields are base64url WITHOUT padding (canonical wire
 * encoding). */
typedef struct {
    char michi_id[MICHI_IDENTITY_MICHI_ID_LEN];
    char public_key[MICHI_IDENTITY_PUBLIC_KEY_B64_LEN];
    char challenge_nonce[MICHI_PAIRING_NONCE_B64_MAX];
    char challenge_signature[MICHI_IDENTITY_SIGNATURE_B64_LEN];
} michi_pairing_peer_t;

/* Result codes of michi_pairing_start(): each maps to exactly one HTTP
 * status in the canonical error map (section 2.7). */
typedef enum {
    MICHI_PAIRING_START_OK = 0,          /*!< 201: session created, PIN shown */
    MICHI_PAIRING_START_WINDOW_CLOSED,   /*!< 403 FORBIDDEN (physical window closed) */
    MICHI_PAIRING_START_INVALID,         /*!< 400 INVALID_REQUEST (nonce/signature/id) */
    MICHI_PAIRING_START_RATE_LIMITED,    /*!< 429 RATE_LIMITED (per IP/global) */
    MICHI_PAIRING_START_INTERNAL,        /*!< 500 INTERNAL_ERROR */
} michi_pairing_start_result_t;

/* Result codes of michi_pairing_status(). */
typedef enum {
    MICHI_PAIRING_STATUS_OK = 0,         /*!< 200: status filled */
    MICHI_PAIRING_STATUS_NOT_FOUND,      /*!< 404 NOT_FOUND (no such session) */
} michi_pairing_status_result_t;

/* Result codes of michi_pairing_confirm(). */
typedef enum {
    MICHI_PAIRING_CONFIRM_OK = 0,        /*!< 200: token issued exactly once */
    MICHI_PAIRING_CONFIRM_NOT_FOUND,     /*!< 404 (unknown or expired session) */
    MICHI_PAIRING_CONFIRM_INVALID,       /*!< 400 (identity mismatch / malformed) */
    MICHI_PAIRING_CONFIRM_PIN_MISMATCH,  /*!< 401 (wrong PIN; attempt consumed) */
    MICHI_PAIRING_CONFIRM_LOCKED,        /*!< 429 (sixth attempt; session consumed) */
    MICHI_PAIRING_CONFIRM_CONFLICT,      /*!< 409 (session already confirmed) */
    MICHI_PAIRING_CONFIRM_INTERNAL,      /*!< 500 (persistence/identity failure) */
} michi_pairing_confirm_result_t;

/* Pairing session status (pair/status contract: pending, confirmed,
 * expired, locked). */
typedef enum {
    MICHI_PAIRING_SESSION_PENDING = 0,
    MICHI_PAIRING_SESSION_CONFIRMED,
    MICHI_PAIRING_SESSION_EXPIRED,
    MICHI_PAIRING_SESSION_LOCKED,
} michi_pairing_session_status_t;

/**
 * @brief Initialize the pairing subsystem: load the controller registry
 *        from NVS (namespace "michi_pairing", key "controllers") and
 *        create the window-expiry timer.
 *
 * A missing/corrupt/foreign-version store (including the phase-10
 * version-1 layout) is treated as EMPTY (warn logged): the component
 * never fails boot over a bad store, it starts unpaired. The count of
 * loaded controllers is logged (`pairing: loaded controllers=%u`).
 *
 * Must be called after init_nvs() (the registry lives in NVS) and after
 * michi_identity_init() (pair/start emits the server identity). The FSM
 * must be initialized (the window close posts FSM events). Safe to call
 * once; repeated calls return ESP_OK (idempotent). On failure app_main
 * continues degraded: no pairing, everything else keeps working.
 *
 * @return ESP_OK; ESP_ERR_NO_MEM if the mutex cannot be created;
 *         esp_timer_create errors are propagated unchanged.
 */
esp_err_t michi_pairing_init(void);

/**
 * @brief Open the pairing window. ONLY from the physical button path
 *        (michi_button, task context) or an equivalent physical gate -
 *        this is the single authorization point of the whole protocol.
 *
 * If a window is already open it is replaced silently (timer restarted,
 * state cleared, NO FSM event: the button posts PAIRING_STARTED itself
 * when it was not already in PAIRING) and a fresh window is opened.
 * Pending pairing sessions and the per-window rate counters are cleared
 * (contract: "Abrir de nuevo reemplaza la ventana previa y elimina
 * sesiones de pairing pendientes"). The one-shot expiry timer
 * (CONFIG_MICHI_PAIRING_WINDOW_SECONDS, monotonic clock) is (re)started;
 * on expiry the window closes itself and posts
 * MICHI_EVENT_PAIRING_WINDOW_CLOSED (PAIRING -> IDLE).
 *
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init; esp_timer errors
 *         are propagated unchanged (the window is NOT opened).
 */
esp_err_t michi_pairing_open_window(void);

/**
 * @brief Whether a pairing window is currently open (monotonic deadline
 *        check; never mutates state).
 *
 * @return true while the window is open and before its deadline.
 */
bool michi_pairing_is_window_open(void);

/**
 * @brief Start a pairing session (POST /pair/start), only inside the
 *        physical window.
 *
 * Validations, in order: window open (WINDOW_CLOSED), per-IP and global
 * rate limits (RATE_LIMITED), peer field formats + Ed25519 signature
 * over the DECODED challenge_nonce bytes under public_key
 * (michi_identity_verify) + michi_id == base64url-nopad(blake3(pubkey))
 * (michi_identity_derive_michi_id) - any failure is INVALID and creates
 * NO session.
 *
 * On success: a fresh UUID v4 session_id, a uniformly-drawn 6-digit PIN
 * (esp_fill_random, rejection sampling), the session deadline (window
 * deadline, monotonic) and an RFC 3339 expires_at. The PIN is handed to
 * the PIN display callback (michi_pairing_set_pin_display_cb) for the
 * local screen and is NOT part of any return value.
 *
 * @param peer                  Controller identity (all fields
 *                              base64url-nopad strings).
 * @param ip                    Source IP of the request (rate limit
 *                              key; NULL maps to a reserved slot).
 * @param out_session_id        Buffer (>= MICHI_PAIRING_SESSION_ID_LEN).
 * @param session_id_len        Size of out_session_id.
 * @param out_expires_at        Buffer (>= MICHI_PAIRING_EXPIRES_AT_LEN).
 * @param expires_len           Size of out_expires_at.
 * @param out_attempts_remaining Receives MICHI_PAIRING_PIN_ATTEMPTS.
 * @return A MICHI_PAIRING_START_* result code.
 */
michi_pairing_start_result_t michi_pairing_start(
    const michi_pairing_peer_t *peer, const char *ip,
    char *out_session_id, size_t session_id_len,
    char *out_expires_at, size_t expires_len,
    uint32_t *out_attempts_remaining);

/**
 * @brief Query a pairing session (GET /pair/status).
 *
 * A pending session past its deadline reports status "expired" (the
 * session record stays until the window is re-opened). Locked sessions
 * report their real remaining attempts (0); the HTTP layer clamps to the
 * schema floor (1) when serializing.
 *
 * @param session_id              Session UUID.
 * @param out_status              Buffer (>= 12 bytes).
 * @param status_len              Size of out_status.
 * @param out_expires_at          Buffer (>= MICHI_PAIRING_EXPIRES_AT_LEN).
 * @param expires_len             Size of out_expires_at.
 * @param out_attempts_remaining  Receives attempts left (0 when locked).
 * @return MICHI_PAIRING_STATUS_OK / MICHI_PAIRING_STATUS_NOT_FOUND.
 */
michi_pairing_status_result_t michi_pairing_status(
    const char *session_id, char *out_status, size_t status_len,
    char *out_expires_at, size_t expires_len,
    uint32_t *out_attempts_remaining);

/**
 * @brief Confirm a pairing session (POST /pair/confirm).
 *
 * Validations, in order: session exists (NOT_FOUND), not confirmed
 * (CONFLICT), not locked (LOCKED - the sixth attempt and beyond answer
 * 429 and the session stays consumed), not expired (NOT_FOUND), michi_id
 * and public_key EXACTLY equal to the pair/start values (INVALID), PIN
 * format (INVALID, no attempt consumed). A wrong well-formed PIN
 * consumes one attempt (PIN_MISMATCH); when the last attempt is consumed
 * the session becomes locked.
 *
 * On success the receiver generates the token (32 bytes esp_fill_random,
 * base64url-nopad, 43 chars), assigns a fresh UUID v4 device_id, stores
 * ONLY the SHA-256 digest of the token (with michi_id, public_key, the
 * default permissions, creation date and last activity) in NVS, and
 * marks the session confirmed. The token is returned exactly ONCE (this
 * call); a later replay gets CONFLICT. A persistence failure returns
 * INTERNAL and leaves the session pending (no token was issued).
 *
 * The PIN and the token are never logged.
 *
 * @param session_id      Session UUID from pair/start.
 * @param pin             Six-digit PIN.
 * @param michi_id        Controller michi_id (must match pair/start).
 * @param public_key      Controller public key (must match pair/start).
 * @param out_token       Buffer (>= MICHI_PAIRING_TOKEN_B64_LEN).
 * @param token_len       Size of out_token.
 * @param out_device_id   Buffer (>= MICHI_PAIRING_DEVICE_ID_LEN).
 * @param device_id_len   Size of out_device_id.
 * @return A MICHI_PAIRING_CONFIRM_* result code.
 */
michi_pairing_confirm_result_t michi_pairing_confirm(
    const char *session_id, const char *pin, const char *michi_id,
    const char *public_key, char *out_token, size_t token_len,
    char *out_device_id, size_t device_id_len);

/**
 * @brief Validate a bearer token (43-char base64url-nopad) against the
 *        stored registry (constant time) and return the owning device id
 *        + its permissions.
 *
 * The token's SHA-256 digest is compared in constant time against EVERY
 * slot (empty slots against a fixed zero digest), with NO early return
 * on match: the time does not reveal which controller matched nor how
 * many are stored. On success the controller's last activity timestamp
 * is refreshed in RAM and persisted at most once per
 * MICHI_PAIRING_ACTIVITY_PERSIST_SECONDS (flash-wear guard).
 *
 * @param token          The base64url-nopad token to validate.
 * @param out_device_id  Buffer (>= MICHI_PAIRING_DEVICE_ID_LEN).
 * @param id_len         Size of out_device_id.
 * @param out_permissions Receives the controller's permission bitmask.
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init;
 *         ESP_ERR_INVALID_ARG (malformed token / too-small buffers);
 *         ESP_ERR_NOT_FOUND (no controller matches); NVS errors from
 *         the activity refresh are swallowed (validation still succeeds).
 */
esp_err_t michi_pairing_validate_token(const char *token,
                                       char *out_device_id,
                                       size_t id_len,
                                       uint32_t *out_permissions);

/**
 * @brief Validate a token (as michi_pairing_validate_token) and check a
 *        permission bit.
 *
 * Convenience wrapper for permission gates: false on any validation
 * failure (malformed token, no match, before init) - callers must not
 * distinguish "invalid" from "forbidden" here; use
 * michi_pairing_validate_token() when the error matters.
 *
 * @param token The base64url-nopad token.
 * @param perm  One MICHI_PERM_* bit (or a mask).
 * @return true when the token matches a stored controller whose
 *         permissions include perm.
 */
bool michi_pairing_has_permission(const char *token, uint32_t perm);

/**
 * @brief Revoke a controller: remove its entry and persist.
 *
 * @param device_id The device id assigned at confirm time (UUID v4).
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init;
 *         ESP_ERR_INVALID_ARG (malformed id); ESP_ERR_NOT_FOUND
 *         (no such controller); NVS errors propagated unchanged.
 */
esp_err_t michi_pairing_revoke(const char *device_id);

/**
 * @brief List stored controller device ids as "id1,id2" (no secrets, no
 *        permissions, no digests).
 *
 * Pre-init contract as the rest of the API: the registry is not valid
 * before michi_pairing_init().
 *
 * @param out     Buffer for the list.
 * @param out_len Size of out.
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init;
 *         ESP_ERR_INVALID_ARG (NULL/zero-length buffer);
 *         ESP_ERR_INVALID_SIZE when the list does not fit (buffer left
 *         in an unspecified partial state).
 */
esp_err_t michi_pairing_list(char *out, size_t out_len);

/**
 * @brief Factory-reset hook: erase the whole controller registry (NVS
 *        key + in-RAM copy). A full-device factory reset erases the
 *        whole NVS partition and achieves the same; this is the pairing-
 *        side guarantee when only the identity/registry must be wiped.
 *
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init; NVS errors
 *         propagated unchanged.
 */
esp_err_t michi_pairing_erase_all(void);

/**
 * @brief PIN display callback: called with the 6-digit PIN when a
 *        pairing session is created, and with NULL when the window
 *        closes (the screen must clear). Runs in task context (the HTTP
 *        handler), never from an ISR or the timer task.
 *
 * @param pin The PIN string, or NULL to clear.
 * @param ctx Opaque context passed to michi_pairing_set_pin_display_cb.
 */
typedef void (*michi_pairing_pin_display_cb_t)(const char *pin, void *ctx);

/**
 * @brief Register the PIN display callback (single slot; NULL clears).
 *
 * app_main registers the screen glue so the component never depends on
 * michi_display. The pairing host tests register a spy.
 */
void michi_pairing_set_pin_display_cb(michi_pairing_pin_display_cb_t cb,
                                      void *ctx);

/**
 * @brief Close the pairing window and post MICHI_EVENT_PAIRING_WINDOW_CLOSED
 *        (PAIRING -> IDLE in the FSM) - but only if it was open.
 *
 * Clears the sessions, the per-window counters, the PIN display (NULL
 * callback) and cancels the expiry timer. Idempotent: closing an
 * already-closed window returns ESP_OK without posting.
 *
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init.
 */
esp_err_t michi_pairing_close_window(void);

/**
 * @brief Shut the pairing subsystem down: cancel + delete the expiry
 *        timer, close the window if open (no FSM event: the bus may be
 *        down; the close is logged).
 *
 * Idempotent: a second call when the subsystem is already off returns
 * ESP_OK. MUST be called from regular task context: deleting the timer
 * from inside its own callback (the esp_timer task) is not supported.
 *
 * @return ESP_OK.
 */
esp_err_t michi_pairing_shutdown(void);

#ifdef __cplusplus
}
#endif
