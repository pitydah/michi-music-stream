#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pairing & security (phase 10): controller registry + the pairing window.
 *
 * Architecture (see firmware/README.md, Pairing & security section):
 * - The physical button is the ONLY authority that opens the pairing
 *   window: michi_pairing_open_window() is called exclusively from the
 *   button path (michi_button, task context) or an equivalent physical
 *   gate. There is NO network-visible API that opens it - the phase-12
 *   HTTP handlers can only request/confirm challenges INSIDE a window
 *   already opened by the button.
 * - A window is a short-lived (MICHI_PAIRING_WINDOW_SECONDS) pairing
 *   session: it issues challenges (16 random bytes from esp_fill_random,
 *   hex, 32 chars) bound to the requesting initiator id (single-slot: one
 *   active challenge per window; a new get_challenge overwrites the
 *   previous challenge+owner pair) and accepts one confirmation
 *   (challenge + initiator id + token) - only from the SAME id that
 *   issued the challenge. The window closes on expiry (one-shot
 *   esp_timer), on a successful pairing, on confirm-attempt exhaustion
 *   (anti brute force) or on an explicit close - always posting
 *   MICHI_EVENT_PAIRING_WINDOW_CLOSED (PAIRING -> IDLE in the FSM).
 * - Tokens are receiver-checked by their SHA-256: only the digest is
 *   stored (NVS namespace "michi_pairing", single versioned blob), and
 *   validation compares digests in CONSTANT TIME (mbedtls_ct_memcmp)
 *   against every slot, without early return - the timing never reveals
 *   which controller matched, nor how many are stored (empty slots are
 *   compared against a fixed zero digest).
 * - Rate limits per window: MICHI_PAIRING_MAX_CHALLENGES_PER_WINDOW
 *   issues and MICHI_PAIRING_MAX_CONFIRM_ATTEMPTS failed confirmations;
 *   exceeding the confirmation limit CLOSES the window (log + FSM event).
 * - Attempts contract: every failed confirmation consumes one of the
 *   window's attempts EXCEPT a malformed initiator id and no-proof-issued
 *   (no challenge was issued for the window), which are rejected without
 *   consuming - they are not authentication attempts.
 * - Zero secrets in logs: challenge/token/digest values are NEVER logged;
 *   logs carry only controller ids (not secret) and counters.
 * - Permissions are a per-controller bitmask granted at pairing time
 *   (MICHI_PERM_DEFAULT). The elevated bits (controller admin, OTA,
 *   factory reset) are documented as NOT granted by default - the grant
 *   management flow is a future phase.
 *
 * Persistence: the registry lives in NVS as ONE versioned blob (key
 * "controllers", namespace "michi_pairing"). Blob layout
 * (MICHI_PAIRING_BLOB_VERSION = 1):
 *   offset 0:  u32 version (MICHI_PAIRING_BLOB_VERSION)
 *   offset 4:  u32 count
 *   offset 8:  count entries, each 80 bytes, field order:
 *                controller_id[32]  (NUL-terminated, <= 31 chars)
 *                digest[32]         (SHA-256 of the token, never the token)
 *                created_unix       (int64, uptime seconds until a wall clock lands)
 *                permissions        (u32 bitmask)
 *                reserved           (u32, explicit tail padding: layout stability,
 *                                    deterministic persisted bytes)
 *   The 8-byte header keeps the entries 8-aligned (created_unix sits at
 *   offset 64 within the entry). Slots [count..MAX) are always zeroed
 *   before persisting (no revoked digests retained on flash), and the
 *   layout is _Static_assert-guarded. Every mutation rewrites the whole
 *   blob (nvs_set_blob + commit) and propagates NVS errors.
 */

/* Controller permissions (bitmask, uint32_t). */
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

/**
 * @brief Initialize the pairing subsystem: load the controller registry
 *        from NVS (namespace "michi_pairing", key "controllers") and
 *        create the window-expiry timer.
 *
 * A missing/corrupt/version-mismatched store is treated as EMPTY (warn
 * logged): the component never fails boot over a bad store, it starts
 * unpaired. Individual corrupt entries (id not NUL-terminated within
 * 31 chars) are dropped without rejecting the whole store (warn logged,
 * persisted by the next mutation). The loaded blob ALWAYS carries
 * MICHI_PAIRING_BLOB_VERSION in RAM (it is written on every load path),
 * so the next persist survives the round-trip version check. The count
 * of loaded controllers is logged (`pairing: loaded controllers=%u`).
 *
 * Must be called after init_nvs() (the registry lives in NVS); the FSM
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
 * If a window is already open it is closed silently (timer cancelled,
 * state cleared, NO FSM event: the caller re-opens immediately and posts
 * PAIRING_STARTED; a close event here would race it) and a fresh window
 * is opened. The one-shot expiry timer (MICHI_PAIRING_WINDOW_SECONDS) is
 * (re)started; on expiry the window closes itself and posts
 * MICHI_EVENT_PAIRING_WINDOW_CLOSED (PAIRING -> IDLE).
 *
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init; esp_timer errors
 *         are propagated unchanged (the window is NOT opened).
 */
esp_err_t michi_pairing_open_window(void);

/**
 * @brief Whether a pairing window is currently open.
 *
 * The one-shot timer owns the expiry close + FSM event; this getter also
 * returns false once the deadline passed but the timer has not fired yet
 * (the close follows within one timer tick) - it never mutates state.
 *
 * @return true while the window is open and before its deadline.
 */
bool michi_pairing_is_window_open(void);

/**
 * @brief Issue a challenge for an initiator, bound to the current window.
 *
 * Generates 16 random bytes (esp_fill_random), encodes them as 32
 * lowercase hex chars + NUL into out_hex (out_len must be >= 33) and
 * records one issue for the window's challenge rate limit.
 *
 * Single-slot semantics: ONE active challenge per window, bound to the
 * initiator id that requested it. A new get_challenge (same window or a
 * re-opened one) OVERWRITES the previous challenge+owner pair. The
 * challenge dies with the window (close/expiry clears it); confirm()
 * only accepts the id that issued it.
 *
 * The challenge VALUE is never logged; the owner id (not secret) appears
 * in the `owner=` log field.
 *
 * @param initiator_id The id that will confirm the challenge (1..31
 *                     chars, alphanumeric + '-'); stored as the
 *                     challenge owner.
 * @param out_hex  Buffer for the hex challenge.
 * @param out_len  Size of out_hex.
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init or with no open
 *         window; ESP_ERR_INVALID_ARG for a too-small buffer or a
 *         malformed initiator id (not consumed);
 *         ESP_ERR_TIMEOUT when the window exhausted its issue limit
 *         (MICHI_PAIRING_MAX_CHALLENGES_PER_WINDOW).
 */
esp_err_t michi_pairing_get_challenge(const char *initiator_id,
                                      char *out_hex, size_t out_len);

/**
 * @brief Confirm a pairing: prove the challenge, register the initiator
 *        and its token, grant the default permissions.
 *
 * Validations, in order: window open (INVALID_STATE), initiator id format
 * (1..31 chars, alphanumeric + '-', INVALID_ARG - NOT consumed), challenge
 * format (32 hex chars, INVALID_ARG) compared to the issued challenge in
 * constant time (mismatch = ESP_ERR_NOT_FOUND), the initiator id vs. the
 * challenge owner (INVALID_STATE, plain strcmp - the id is not secret:
 * the challenge is single-slot and only the id that issued it may
 * confirm), token format (64 hex chars, INVALID_ARG) whose SHA-256 digest
 * becomes the stored credential.
 *
 * Attempts contract: EVERY failed confirmation consumes one of the
 * window's MICHI_PAIRING_MAX_CONFIRM_ATTEMPTS attempts EXCEPT a
 * malformed initiator id and no-proof-issued (no challenge issued for
 * the window), which are rejected without consuming - they are not
 * authentication attempts. When the limit is exceeded the window CLOSES
 * (log + FSM event) and ESP_ERR_TIMEOUT is returned.
 *
 * The initiator id is the controller identity: if a controller with the
 * same id is already stored, its entry is REUSED (token replaced,
 * permissions reset to MICHI_PERM_DEFAULT, created updated) - re-pairing
 * rotates the credential. Otherwise a new entry is appended (up to
 * MICHI_PAIRING_MAX_CONTROLLERS). The store is written to NVS (blob +
 * commit) before the in-RAM registry is updated; NVS errors are returned
 * and the in-RAM state is unchanged.
 *
 * On success the window closes (MICHI_EVENT_PAIRING_WINDOW_CLOSED, so the
 * FSM leaves PAIRING) and the granted controller id + permissions are
 * returned.
 *
 * @param challenge_hex     The hex challenge previously issued by
 *                          michi_pairing_get_challenge().
 * @param initiator_id      Client-claimed identity (1..31 chars,
 *                          alphanumeric + '-'); it becomes the stored
 *                          controller id and MUST match the id the
 *                          challenge was issued for.
 * @param token_hex         The controller credential: 64 hex chars (32
 *                          bytes of client randomness). Stored ONLY as
 *                          its SHA-256 digest.
 * @param out_controller_id Buffer for the granted controller id
 *                          (>= 32 bytes).
 * @param out_id_len        Size of out_controller_id.
 * @param out_permissions   Receives the granted permission bitmask.
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init, window closed, no
 *         challenge issued yet for the window, or initiator id != the
 *         challenge owner (attempt consumed); ESP_ERR_INVALID_ARG
 *         (malformed input - malformed initiator id NOT consumed,
 *         malformed challenge/token consumed; too-small buffers);
 *         ESP_ERR_NOT_FOUND (wrong challenge - attempt consumed);
 *         ESP_ERR_NO_MEM (registry full); ESP_ERR_TIMEOUT (window closed
 *         by attempt exhaustion); NVS errors propagated unchanged.
 */
esp_err_t michi_pairing_confirm(const char *challenge_hex,
                                const char *initiator_id,
                                const char *token_hex,
                                char *out_controller_id,
                                size_t out_id_len,
                                uint32_t *out_permissions);

/**
 * @brief Validate a token against the stored registry (constant time)
 *        and return the owning controller + its permissions.
 *
 * The token's SHA-256 digest is compared with mbedtls_ct_memcmp against
 * EVERY slot (empty slots against a fixed zero digest), with NO early
 * return on match: the time does not reveal which controller matched nor
 * how many controllers are stored (trade-off: O(MAX_CONTROLLERS)
 * comparisons per validation).
 *
 * @param token_hex         The 64-hex-char token to validate.
 * @param out_controller_id Buffer for the owning controller id
 *                          (>= 32 bytes).
 * @param out_id_len        Size of out_controller_id.
 * @param out_permissions   Receives the controller's permission bitmask.
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init;
 *         ESP_ERR_INVALID_ARG (malformed token / too-small buffers);
 *         ESP_ERR_NOT_FOUND (no controller matches).
 */
esp_err_t michi_pairing_validate_token(const char *token_hex,
                                       char *out_controller_id,
                                       size_t out_id_len,
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
 * @param token_hex The 64-hex-char token.
 * @param perm      One MICHI_PERM_* bit (or a mask).
 * @return true when the token matches a stored controller whose
 *         permissions include perm.
 */
bool michi_pairing_has_permission(const char *token_hex, uint32_t perm);

/**
 * @brief Revoke a controller: remove its entry and persist.
 *
 * @param controller_id The controller id (as granted by confirm).
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init;
 *         ESP_ERR_INVALID_ARG (malformed id); ESP_ERR_NOT_FOUND
 *         (no such controller); NVS errors propagated unchanged.
 */
esp_err_t michi_pairing_revoke(const char *controller_id);

/**
 * @brief List stored controller ids as "id1,id2" (no secrets, no
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
 * @brief Close the pairing window and post MICHI_EVENT_PAIRING_WINDOW_CLOSED
 *        (PAIRING -> IDLE in the FSM) - but only if it was open.
 *
 * Clears the challenge and the per-window counters, cancels the expiry
 * timer. Idempotent: closing an already-closed window returns ESP_OK
 * without posting. This is the single close path used by the expiry
 * timer, the confirm success, the attempt-exhaustion close and the
 * explicit API call.
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
 * Teardown order (F3, deadlock-safe vs. the timer callback): set the
 * internal s_teardown flag (the callback checks it BEFORE taking the
 * mutex and returns - it can never block on, or take, a mutex that is
 * about to be deleted), stop the timer, take the mutex, delete the
 * timer, release the mutex, and ONLY THEN delete the mutex and clear
 * s_initialized.
 *
 * @return ESP_OK.
 */
esp_err_t michi_pairing_shutdown(void);

#ifdef __cplusplus
}
#endif
