#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Error text buffer size (single source of truth: michi_ota.c writes it,
 * the HTTP diagnostics handler reads it with this bound). */
#define MICHI_OTA_ERR_MAX 96

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Signed OTA updates (phase 13): manifest fetch + signature
 *        verification + streaming download + A/B partition swap with
 *        boot-time rollback.
 *
 * Flow (see firmware/README.md, OTA section):
 *  - POST /api/v1/receiver/updates {url} -> michi_ota_start(): the URL
 *    points at a SIGNED MANIFEST (JSON, RSA-2048 PKCS#1 v1.5 SHA-256
 *    verified against the embedded public key michi_ota_pubkey_der).
 *  - The manifest is validated field by field: board exact match with the
 *    product profile, strict semver (version > current, version >=
 *    min_version - downgrade prevention), https:// binary URL, 64-hex
 *    sha256, valid signature over "version|board|min_version|url|sha256".
 *  - The binary is streamed (4 KB chunks) with esp_http_client +
 *    esp_crt_bundle_attach (CA-verified TLS, no CN skip), written with
 *    esp_ota_begin/esp_ota_write (OTA_SIZE_UNKNOWN) into the next update
 *    partition, its SHA-256 computed at runtime and compared to the
 *    manifest, then esp_ota_end + esp_ota_set_boot_partition + restart.
 *  - Boot-time rollback: with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE the
 *    OTA'd app boots in ESP_OTA_IMG_PENDING_VERIFY; app_main calls
 *    michi_ota_boot_selftest_done() after the board self-test and the
 *    profile build. Marking valid (cancel rollback) when the self-test
 *    passed; logging + esp_restart() when it failed so the bootloader
 *    rolls back to the previous partition.
 *
 * Security invariants:
 *  - TLS: https:// enforced at validation (http/ftp/arbitrary rejected),
 *    no userinfo, cert verified with the CA bundle - never skipped.
 *  - Integrity: the binary sha256 is inside the SIGNED manifest, so a
 *    tampered binary is rejected before it is ever booted; the signature
 *    key is the embedded public key (private key never in the repo).
 *  - The URL is not a secret, but query strings are never logged
 *    (the URL may carry a token); logs carry host + path length only.
 *
 * Threading: michi_ota_start() validates synchronously and spawns a
 * dedicated task (MICHI_OTA_STACK_BYTES) that performs the whole update;
 * get_state() is safe from any task; boot_selftest_done() is boot-time.
 */

/**
 * @brief OTA lifecycle state (michi_ota_get_state).
 */
typedef enum {
    MICHI_OTA_IDLE = 0,        /*!< No update in progress */
    MICHI_OTA_FETCHING_MANIFEST, /*!< Downloading the signed manifest */
    MICHI_OTA_VALIDATING,      /*!< Parsing + signature/board/version checks */
    MICHI_OTA_DOWNLOADING,     /*!< Streaming the firmware binary */
    MICHI_OTA_VERIFYING,       /*!< Runtime SHA-256 vs manifest */
    MICHI_OTA_APPLYING,        /*!< esp_ota_end + set_boot_partition */
    MICHI_OTA_DONE,            /*!< Image staged, restarting */
    MICHI_OTA_FAILED,          /*!< Terminal failure (see err buffer) */
} michi_ota_state_t;

/**
 * @brief Initialize the OTA subsystem: logs the running partition + its
 *        image state (ESP_OTA_IMG_*) at boot.
 *
 * Must be called after michi_state_init() (the update path requests
 * MICHI_STATE_UPDATING) and after michi_session_init() (it force-closes
 * the active session). Boot-time marking (michi_ota_boot_selftest_done)
 * does NOT depend on this init - it must run even if init failed.
 * Idempotent; repeated calls return ESP_OK.
 *
 * @return ESP_OK (the subsystem keeps no allocations at init; the update
 *         task is created per michi_ota_start()).
 */
esp_err_t michi_ota_init(void);

/**
 * @brief Start a signed update from a manifest URL.
 *
 * Synchronous validation (fast, no network): URL scheme https:// with no
 * userinfo and host non-empty, length <= CONFIG_MICHI_OTA_URL_MAX; a
 * violation returns ESP_ERR_INVALID_ARG with a log - nothing is queued.
 * The FSM is requested to MICHI_STATE_UPDATING (valid from IDLE/PLAYING/
 * PAUSED; from any other state the update still proceeds and the FSM
 * follows best-effort) and an active session is force-closed
 * (michi_session_abort, phase-13 privileged call - the session credential
 * is never persisted so OTA cannot present it).
 *
 * On success an OTA task is spawned and ESP_OK is returned (the HTTP
 * handler answers 202); the download/validation/apply pipeline runs
 * asynchronously - poll michi_ota_get_state(). While busy a second start
 * returns ESP_ERR_INVALID_STATE (the busy check and the task creation
 * are atomic under one critical section - no double task on a concurrent
 * start).
 *
 * A start is rejected with ESP_ERR_INVALID_STATE when the active session
 * could not be force-closed (michi_session_abort failed): the update does
 * NOT start with a live session - the abort result is authoritative, no
 * retry is attempted (a session that survived one privileged abort call
 * is not more likely to die on a second; the operator can retry the
 * start).
 *
 * @param url Manifest URL (https://, <= CONFIG_MICHI_OTA_URL_MAX chars).
 * @return ESP_OK (started); ESP_ERR_INVALID_STATE (already busy, or the
 *         active session could not be aborted - update not started);
 *         ESP_ERR_NOT_ALLOWED (running image is in PENDING_VERIFY - the
 *         boot self-test must mark it valid first; the API answers 409
 *         'pending_verify'); ESP_ERR_INVALID_ARG (URL rejected);
 *         ESP_ERR_NO_MEM (task/queue allocation).
 */
esp_err_t michi_ota_start(const char *url);

/**
 * @brief Snapshot the OTA state (any task).
 *
 * @param state   Out: lifecycle state (never NULL).
 * @param percent Out: progress 0-100 (5: manifest fetched, 10:
 *                validating, 10-85: download, 90: verifying, 95:
 *                applying, 100: done). With a content-length download the
 *                percent advances by bytes; with chunked transfer
 *                (no content-length) it stays at 10 from the download
 *                anchor until the verify step. NULL allowed.
 * @param err     Out: error text on MICHI_OTA_FAILED ("" otherwise),
 *                NUL-terminated; NULL allowed.
 * @param err_len Size of err.
 * @return ESP_OK; ESP_ERR_INVALID_ARG on NULL state.
 */
esp_err_t michi_ota_get_state(michi_ota_state_t *state, int *percent,
                              char *err, size_t err_len);

/**
 * @return true while an update task is running (from start to restart or
 *         terminal failure). MICHI_OTA_FAILED is not busy.
 */
bool michi_ota_busy(void);

/**
 * @brief Boot-time rollback decision, called once from app_main after the
 *        board self-test + product profile (phase 13).
 *
 * Criterion (documented contract): pass the BOARD self-test overall
 * (chip/flash/psram/display/backlight). A DIAGNOSTIC profile (no DAC
 * detected) does NOT block the mark: a DAC-less unit is a legitimate
 * hardware option, not a new-image defect.
 *
 * Action, only when the RUNNING partition is in ESP_OTA_IMG_PENDING_VERIFY
 * (first boot after an OTA):
 *  - selftest_ok:  esp_ota_mark_app_valid_cancel_rollback() + log.
 *  - !selftest_ok: log the verdict + esp_restart() - the bootloader rolls
 *    back to the previous partition (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
 * Any other image state: no-op (log only) - nothing to mark or roll back.
 *
 * Independent of michi_ota_init(): must run even when init failed.
 *
 * @param selftest_ok true = the board self-test passed (st.overall).
 * @return ESP_OK; ESP_ERR_INVALID_STATE if the running partition state
 *         cannot be read (log-only, boot continues).
 */
esp_err_t michi_ota_boot_selftest_done(bool selftest_ok);

#ifdef __cplusplus
}
#endif
