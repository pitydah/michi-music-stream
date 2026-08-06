#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"

#include "mbedtls/constant_time.h"
#include "mbedtls/sha256.h"

#include "michi_pairing.h"
#include "michi_state.h"

#define TAG "michi_pairing"

#define MICHI_PAIRING_NVS_NAMESPACE "michi_pairing"
#define MICHI_PAIRING_NVS_KEY "controllers"
#define MICHI_PAIRING_BLOB_VERSION 1u

#define MICHI_PAIRING_CHALLENGE_BYTES 16
#define MICHI_PAIRING_TOKEN_BYTES 32
#define MICHI_PAIRING_DIGEST_BYTES 32
#define MICHI_PAIRING_ID_MAX 31
#define MICHI_PAIRING_ID_LEN (MICHI_PAIRING_ID_MAX + 1) /* + NUL */

#define MICHI_PAIRING_CHALLENGE_HEX_LEN (2 * MICHI_PAIRING_CHALLENGE_BYTES)
#define MICHI_PAIRING_TOKEN_HEX_LEN (2 * MICHI_PAIRING_TOKEN_BYTES)

/* One persisted controller. Field order is deliberate (no padding
 * between fields: controller_id[32] + digest[32] + int64 (8-aligned at
 * offset 64) + uint32 + explicit reserved tail = 80 bytes - the reserved
 * field keeps the persisted bytes deterministic instead of carrying
 * uninitialized tail padding). _Static_assert guards the layout so the
 * persisted blob format cannot silently change. */
typedef struct {
    uint8_t controller_id[MICHI_PAIRING_ID_LEN]; /* NUL-terminated, <= 31 chars */
    uint8_t digest[MICHI_PAIRING_DIGEST_BYTES];  /* SHA-256 of the token (never the token) */
    int64_t created_unix;                        /* uptime seconds until a wall clock lands */
    uint32_t permissions;
    uint32_t reserved;                           /* explicit tail padding: layout stability */
} michi_controller_entry_t;

_Static_assert(sizeof(michi_controller_entry_t) == 80,
               "controller entry layout changed; the persisted blob format breaks");

typedef struct {
    uint32_t version;
    uint32_t count;
    michi_controller_entry_t controllers[CONFIG_MICHI_PAIRING_MAX_CONTROLLERS];
} michi_pairing_blob_t;

_Static_assert(sizeof(michi_pairing_blob_t) ==
                   8 + CONFIG_MICHI_PAIRING_MAX_CONTROLLERS * 80,
               "blob layout changed; the persisted blob format breaks");

static SemaphoreHandle_t s_mutex;
static esp_timer_handle_t s_timer;
static volatile bool s_initialized;
/* Teardown flag (shutdown): window_timer_cb checks it BEFORE taking the
 * mutex - a callback already dispatched when shutdown runs must return
 * without touching the mutex (shutdown deletes it). Set before
 * esp_timer_stop, cleared by a later init. */
static volatile bool s_teardown;
static bool s_window_open;
/* Window opened at (esp_timer_get_time, us): diagnostic reference, and
 * the deadline check that keeps the getters honest during the tiny
 * window between the deadline and the timer callback. */
static int64_t s_window_opened_us;
static uint8_t s_challenge[MICHI_PAIRING_CHALLENGE_BYTES];
/* The initiator id the active challenge was issued for (single-slot:
 * one active challenge per window; a new get_challenge overwrites the
 * previous one). confirm() only accepts that same id. Not secret: the
 * plain strcmp comparison is documented (F2). */
static char s_challenge_owner[MICHI_PAIRING_ID_LEN];
static uint32_t s_challenge_issued;
static uint32_t s_confirm_failures;
static michi_pairing_blob_t s_blob;

/* --- constant-time comparisons --------------------------------------- */

/* mbedtls_ct_memcmp (mbedtls/constant_time.h, mbedTLS 3.6 in IDF 5.3):
 * constant-time buffer comparison - time is independent of the data and
 * of equality (it does depend on n, which is fixed here: digests are
 * always 32 bytes, so no length oracle exists). */
static bool ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    return mbedtls_ct_memcmp(a, b, n) == 0;
}

/* --- hex helpers ------------------------------------------------------ */

static uint8_t hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return (uint8_t)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (uint8_t)(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return (uint8_t)(c - 'A' + 10);
    }
    return 0xff;
}

static bool hex_decode(const char *src, size_t src_len, uint8_t *dst,
                       size_t dst_len)
{
    if (src == NULL || src_len != 2 * dst_len) {
        return false;
    }
    for (size_t i = 0; i < dst_len; i++) {
        const uint8_t hi = hex_val(src[2 * i]);
        const uint8_t lo = hex_val(src[2 * i + 1]);
        if (hi > 15 || lo > 15) {
            return false;
        }
        dst[i] = (uint8_t)((hi << 4) | lo);
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

static bool id_valid(const char *id)
{
    if (id == NULL) {
        return false;
    }
    const size_t len = strlen(id);
    if (len == 0 || len > MICHI_PAIRING_ID_MAX) {
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

/* --- digest ----------------------------------------------------------- */

static esp_err_t digest_of_token(const char *token_hex, uint8_t *out)
{
    uint8_t raw[MICHI_PAIRING_TOKEN_BYTES];
    if (!hex_decode(token_hex, MICHI_PAIRING_TOKEN_HEX_LEN, raw,
                    sizeof(raw))) {
        return ESP_ERR_INVALID_ARG;
    }
    /* One-shot SHA-256 (mbedtls/sha256.h, not deprecated in mbedTLS 3.6
     * of IDF 5.3). The digest - never the token - is what gets stored. */
    if (mbedtls_sha256(raw, sizeof(raw), out, 0) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* --- persistence ------------------------------------------------------ */

static esp_err_t persist_blob(const michi_pairing_blob_t *blob)
{
    /* Deterministic blob: zero the slots [count..MAX) before persisting so
     * a revoked/compacted entry's digest can never survive on flash (the
     * in-RAM copy never reads past `count`, but the stored bytes must not
     * retain them either). Written to a local copy: the caller's blob is
     * not mutated. */
    michi_pairing_blob_t tmp = *blob;
    memset(&tmp.controllers[blob->count], 0,
           (CONFIG_MICHI_PAIRING_MAX_CONTROLLERS - blob->count) *
               sizeof(tmp.controllers[0]));

    nvs_handle_t h;
    esp_err_t err = nvs_open(MICHI_PAIRING_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pairing: store_open_failed err=%s",
                 esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(h, MICHI_PAIRING_NVS_KEY, &tmp, sizeof(tmp));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pairing: persist_failed err=%s", esp_err_to_name(err));
    }
    return err;
}

/* Validate the loaded entries: every controller_id must be NUL-terminated
 * within 31 chars (strnlen bounded by the fixed slot, so a corrupt id can
 * never make the downstream strlen/strcmp reads run past the buffer). An
 * invalid entry is DROPPED (compacted + count--, warn logged) instead of
 * rejecting the whole store: one corrupt slot must not wipe the registry.
 * The cleanup is persisted by the next mutation (confirm/revoke). */
static void sanitize_loaded_blob(void)
{
    for (size_t i = 0; i < s_blob.count;) {
        const char *id = (const char *)s_blob.controllers[i].controller_id;
        const size_t id_len = strnlen(id, MICHI_PAIRING_ID_LEN);
        if (id_len == 0 || id_len > MICHI_PAIRING_ID_MAX) {
            ESP_LOGW(TAG, "pairing: store_corrupt_entry slot=%u (dropped)",
                     (unsigned)i);
            memmove(&s_blob.controllers[i], &s_blob.controllers[i + 1],
                    (s_blob.count - i - 1) * sizeof(s_blob.controllers[0]));
            s_blob.count--;
        } else {
            i++;
        }
    }
}

static void load_blob(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MICHI_PAIRING_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_blob.version = MICHI_PAIRING_BLOB_VERSION;
        s_blob.count = 0;
        ESP_LOGI(TAG, "pairing: loaded controllers=%u", (unsigned)0u);
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pairing: store_read_failed err=%s (starting empty)",
                 esp_err_to_name(err));
        s_blob.version = MICHI_PAIRING_BLOB_VERSION;
        s_blob.count = 0;
        return;
    }

    size_t len = sizeof(s_blob);
    err = nvs_get_blob(h, MICHI_PAIRING_NVS_KEY, &s_blob, &len);
    nvs_close(h);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_blob.version = MICHI_PAIRING_BLOB_VERSION;
        s_blob.count = 0;
        ESP_LOGI(TAG, "pairing: loaded controllers=%u", (unsigned)0u);
        return;
    }
    /* Corrupt, truncated or foreign-version store: start EMPTY instead of
     * trusting it (the next mutation overwrites it). */
    if (err != ESP_OK || len != sizeof(s_blob) ||
        s_blob.version != MICHI_PAIRING_BLOB_VERSION ||
        s_blob.count > CONFIG_MICHI_PAIRING_MAX_CONTROLLERS) {
        ESP_LOGW(TAG, "pairing: store_corrupt controllers=0 (starting empty)");
        memset(&s_blob, 0, sizeof(s_blob));
        s_blob.version = MICHI_PAIRING_BLOB_VERSION;
        return;
    }
    sanitize_loaded_blob();
    ESP_LOGI(TAG, "pairing: loaded controllers=%u", (unsigned)s_blob.count);
}

/* --- window ----------------------------------------------------------- */

/* Post an FSM event with one bounded retry (the button's post_with_retry
 * pattern): ESP_ERR_TIMEOUT means the event queue is full (transient -
 * the FSM task drains it), so a 50 ms wait + a second attempt covers the
 * usual spike; a second failure drops the event and logs it. NOTE: this
 * can be called from the esp_timer task (window expiry); the 50 ms wait
 * only happens on a full queue, a rare transient - the FSM event must
 * not be lost (a stranded PAIRING state is worse than a brief timer
 * task stall). */
static esp_err_t post_event_with_retry(michi_event_id_t id, uint32_t data)
{
    esp_err_t err = michi_state_post(id, data);
    if (err == ESP_ERR_TIMEOUT) {
        vTaskDelay(pdMS_TO_TICKS(50));
        err = michi_state_post(id, data);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pairing: state_post_failed event=%d err=%s", (int)id,
                 esp_err_to_name(err));
    }
    return err;
}

static bool window_active_locked(void)
{
    if (!s_window_open) {
        return false;
    }
    /* Deadline check without mutation: the one-shot timer owns the close
     * + FSM event; this only keeps the getters honest during the few
     * microseconds between the deadline and the callback. */
    const int64_t deadline =
        s_window_opened_us + (int64_t)CONFIG_MICHI_PAIRING_WINDOW_SECONDS * 1000000;
    return esp_timer_get_time() < deadline;
}

/* The single close path. notify=false: no FSM event (used by
 * open_window's silent re-open and by shutdown, where the bus may be
 * down); the close is always logged with the per-window counters. */
static void window_close_locked(const char *reason, bool notify)
{
    if (!s_window_open) {
        return;
    }
    const uint32_t issued = s_challenge_issued;
    const uint32_t failed = s_confirm_failures;
    esp_timer_stop(s_timer);
    s_window_open = false;
    memset(s_challenge, 0, sizeof(s_challenge));
    s_challenge_owner[0] = '\0';
    s_challenge_issued = 0;
    s_confirm_failures = 0;
    ESP_LOGI(TAG, "pairing: window=closed reason=%s issued=%u failed=%u",
             reason, (unsigned)issued, (unsigned)failed);
    if (notify) {
        post_event_with_retry(MICHI_EVENT_PAIRING_WINDOW_CLOSED, 0);
    }
}

static void window_timer_cb(void *arg)
{
    (void)arg;
    /* Teardown race (F3): if shutdown is in progress, return WITHOUT
     * touching the mutex - shutdown stops the timer and deletes the mutex
     * after this flag is set, and taking a deleted mutex is undefined.
     * esp_timer_stop on a one-shot timer that already fired is a no-op,
     * so this check is the ONLY thing between a dispatched callback and
     * the teardown. */
    if (s_teardown) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* Stale-callback guard (F4): re-validate the deadline under the mutex.
     * The window may have been re-opened after this timer fired
     * (esp_timer_stop is a no-op on an already-dispatched one-shot, and
     * esp_timer_start_once re-arms the SAME timer): if the deadline has
     * not passed for the CURRENT window, this callback is stale - it must
     * NOT close the fresh window. */
    const int64_t deadline =
        s_window_opened_us +
        (int64_t)CONFIG_MICHI_PAIRING_WINDOW_SECONDS * 1000000;
    if (esp_timer_get_time() < deadline) {
        xSemaphoreGive(s_mutex);
        return;
    }
    /* Expiry: full close path with the FSM event (PAIRING -> IDLE), with
     * the real reason - NOT the public close_window() which hardcodes
     * "requested" (F8). */
    window_close_locked("expired", true);
    xSemaphoreGive(s_mutex);
}

/* --- confirm failure handling ---------------------------------------- */

/* Every failed confirmation consumes one of the window's attempts; when
 * the limit is EXCEEDED (the max itself stays allowed) the window closes
 * with the FSM event - anti brute force. `result` is returned unless the
 * close happened (then ESP_ERR_TIMEOUT wins: the window is gone). */
static esp_err_t fail_confirm(const char *reason, esp_err_t result)
{
    s_confirm_failures++;
    if (s_confirm_failures > CONFIG_MICHI_PAIRING_MAX_CONFIRM_ATTEMPTS) {
        window_close_locked("attempts_exhausted", true);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGW(TAG, "pairing: confirm_failed reason=%s failed=%u", reason,
             (unsigned)s_confirm_failures);
    return result;
}

/* --- public API -------------------------------------------------------- */

esp_err_t michi_pairing_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_teardown = false;

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "pairing: init mutex_failed err=%s",
                 esp_err_to_name(ESP_ERR_NO_MEM));
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t targs = {
        .callback = window_timer_cb,
        .arg = NULL,
        .name = "michi_pairing",
    };
    esp_err_t err = esp_timer_create(&targs, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pairing: init timer_failed err=%s",
                 esp_err_to_name(err));
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }

    load_blob();
    /* Belt and braces (F1): the version field must ALWAYS be written, on
     * every load path, so the next persist survives the NVS round-trip
     * version check; this also covers a future path that forgets it. */
    s_blob.version = MICHI_PAIRING_BLOB_VERSION;
    s_initialized = true;
    ESP_LOGI(TAG, "subsystem=pairing state=ok phase=10");
    return ESP_OK;
}

esp_err_t michi_pairing_open_window(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_window_open) {
        /* Silent re-open: no FSM event (the caller posts PAIRING_STARTED
         * right after and a close event would race it in the queue). */
        window_close_locked("reopened", false);
    }

    s_window_open = true;
    s_window_opened_us = esp_timer_get_time();
    memset(s_challenge, 0, sizeof(s_challenge));
    s_challenge_owner[0] = '\0';
    s_challenge_issued = 0;
    s_confirm_failures = 0;

    const esp_err_t err = esp_timer_start_once(
        s_timer, (uint64_t)CONFIG_MICHI_PAIRING_WINDOW_SECONDS * 1000000ULL);
    if (err != ESP_OK) {
        s_window_open = false;
        ESP_LOGE(TAG, "pairing: window_timer_start_failed err=%s",
                 esp_err_to_name(err));
        xSemaphoreGive(s_mutex);
        return err;
    }

    ESP_LOGI(TAG, "pairing: window=open seconds=%u",
             (unsigned)CONFIG_MICHI_PAIRING_WINDOW_SECONDS);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

bool michi_pairing_is_window_open(void)
{
    if (!s_initialized) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const bool open = window_active_locked();
    xSemaphoreGive(s_mutex);
    return open;
}

esp_err_t michi_pairing_get_challenge(const char *initiator_id,
                                      char *out_hex, size_t out_len)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_hex == NULL || out_len < MICHI_PAIRING_CHALLENGE_HEX_LEN + 1) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Identity contract error (same rule as confirm): rejected without
     * consuming an issue. */
    if (!id_valid(initiator_id)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!window_active_locked()) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_challenge_issued >= CONFIG_MICHI_PAIRING_MAX_CHALLENGES_PER_WINDOW) {
        ESP_LOGW(TAG, "pairing: window=active issued_limit_reached issued=%u",
                 (unsigned)s_challenge_issued);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_TIMEOUT;
    }

    esp_fill_random(s_challenge, sizeof(s_challenge));
    s_challenge_issued++;
    /* Single-slot semantics: the active challenge is bound to THIS
     * initiator id; a new get_challenge overwrites the previous
     * challenge+owner pair. The id is not secret, so the later confirm
     * comparison uses plain strcmp. */
    strlcpy(s_challenge_owner, initiator_id, sizeof(s_challenge_owner));
    hex_encode(s_challenge, sizeof(s_challenge), out_hex);

    ESP_LOGI(TAG, "pairing: window=active issued=%u owner=%s",
             (unsigned)s_challenge_issued, initiator_id);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t michi_pairing_confirm(const char *challenge_hex,
                                const char *initiator_id,
                                const char *token_hex,
                                char *out_controller_id,
                                size_t out_id_len,
                                uint32_t *out_permissions)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (challenge_hex == NULL || token_hex == NULL || initiator_id == NULL ||
        out_controller_id == NULL || out_permissions == NULL ||
        out_id_len < MICHI_PAIRING_ID_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Identity contract error: rejected without consuming an attempt (it
     * is not an authentication attempt; well-formed ids with wrong
     * challenges still burn attempts - the brute-force cap holds). */
    if (!id_valid(initiator_id)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t challenge[MICHI_PAIRING_CHALLENGE_BYTES];
    uint8_t digest[MICHI_PAIRING_DIGEST_BYTES];

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!window_active_locked()) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    /* No challenge was ever issued for this window: the zeroed challenge
     * buffer must never be confirmable ("0000..." would otherwise match).
     * A protocol-ordering error, not an authentication attempt: it does
     * not consume an attempt. */
    if (s_challenge_issued == 0) {
        ESP_LOGW(TAG, "pairing: confirm_failed reason=no_proof_issued");
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (!hex_decode(challenge_hex, MICHI_PAIRING_CHALLENGE_HEX_LEN, challenge,
                    sizeof(challenge))) {
        const esp_err_t err =
            fail_confirm("malformed_request", ESP_ERR_INVALID_ARG);
        xSemaphoreGive(s_mutex);
        return err;
    }
    /* Constant-time challenge check: never reveal the comparison result
     * by timing. A well-formed wrong challenge is an authentication
     * attempt and consumes one of the window's attempts. */
    if (!ct_equal(challenge, s_challenge, sizeof(challenge))) {
        const esp_err_t err = fail_confirm("proof_mismatch", ESP_ERR_NOT_FOUND);
        xSemaphoreGive(s_mutex);
        return err;
    }
    /* The challenge is bound to the id that issued it (single-slot, F2):
     * only that id may confirm. The id is not secret (strcmp is fine; a
     * ct comparison would protect nothing). Reaching this check requires
     * a VALID proof, so a mismatch is a real protocol violation - it
     * consumes an attempt like every other failed confirmation. */
    if (strcmp(initiator_id, s_challenge_owner) != 0) {
        const esp_err_t err =
            fail_confirm("id_mismatch", ESP_ERR_INVALID_STATE);
        xSemaphoreGive(s_mutex);
        return err;
    }

    const esp_err_t derr = digest_of_token(token_hex, digest);
    if (derr != ESP_OK) {
        /* Both the format error and an engine failure consume an attempt
         * (the cap is about total failed requests); only the reported
         * error differs. */
        const char *reason = (derr == ESP_ERR_INVALID_ARG)
                                 ? "malformed_request"
                                 : "digest_failed";
        const esp_err_t err = fail_confirm(reason, derr);
        xSemaphoreGive(s_mutex);
        return err;
    }

    /* Build the next state in a local copy, persist it, THEN apply it: a
     * failed NVS write never leaves the in-RAM registry half-mutated. */
    michi_pairing_blob_t next = s_blob;
    int slot = -1;
    for (size_t i = 0; i < next.count; i++) {
        if (strcmp((const char *)next.controllers[i].controller_id,
                   initiator_id) == 0) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        if (next.count >= CONFIG_MICHI_PAIRING_MAX_CONTROLLERS) {
            ESP_LOGW(TAG, "pairing: registry_full controllers=%u",
                     (unsigned)next.count);
            xSemaphoreGive(s_mutex);
            return ESP_ERR_NO_MEM;
        }
        slot = (int)next.count;
        next.count++;
        strlcpy((char *)next.controllers[slot].controller_id, initiator_id,
                sizeof(next.controllers[slot].controller_id));
    }
    /* Re-pairing the same id rotates the credential and re-grants the
     * default permission set. */
    memcpy(next.controllers[slot].digest, digest, sizeof(digest));
    next.controllers[slot].permissions = MICHI_PERM_DEFAULT;
    next.controllers[slot].created_unix = esp_timer_get_time() / 1000000;

    const esp_err_t perr = persist_blob(&next);
    if (perr != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return perr;
    }
    s_blob = next;

    strlcpy(out_controller_id, initiator_id, out_id_len);
    *out_permissions = MICHI_PERM_DEFAULT;

    ESP_LOGI(TAG, "pairing: paired controller=%s permissions=%u",
             initiator_id, (unsigned)MICHI_PERM_DEFAULT);
    window_close_locked("paired", true);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t michi_pairing_validate_token(const char *token_hex,
                                       char *out_controller_id,
                                       size_t out_id_len,
                                       uint32_t *out_permissions)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (token_hex == NULL || out_controller_id == NULL ||
        out_permissions == NULL || out_id_len < MICHI_PAIRING_ID_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t digest[MICHI_PAIRING_DIGEST_BYTES];
    const esp_err_t derr = digest_of_token(token_hex, digest);
    if (derr != ESP_OK) {
        return derr;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* Constant-time scan over EVERY slot with NO early return: empty
     * slots compare against a fixed zero digest, so the timing neither
     * reveals which controller matched nor how many are stored. A match
     * on a slot beyond `count` is impossible in practice (a SHA-256
     * preimage of zero) and is discarded below anyway. */
    int matched = -1;
    const uint8_t dummy[MICHI_PAIRING_DIGEST_BYTES] = {0};
    for (size_t i = 0; i < CONFIG_MICHI_PAIRING_MAX_CONTROLLERS; i++) {
        const uint8_t *ref = (i < s_blob.count)
                                 ? s_blob.controllers[i].digest
                                 : dummy;
        if (ct_equal(ref, digest, sizeof(digest))) {
            matched = (int)i;
        }
    }
    if (matched < 0 || (size_t)matched >= s_blob.count) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    strlcpy(out_controller_id,
            (const char *)s_blob.controllers[matched].controller_id,
            out_id_len);
    *out_permissions = s_blob.controllers[matched].permissions;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

bool michi_pairing_has_permission(const char *token_hex, uint32_t perm)
{
    uint32_t perms = 0;
    char id[MICHI_PAIRING_ID_LEN];
    /* Any validation failure (malformed, unknown, before init) is
     * "no permission": the convenience wrapper does not distinguish
     * invalid from forbidden. */
    if (michi_pairing_validate_token(token_hex, id, sizeof(id), &perms) !=
        ESP_OK) {
        return false;
    }
    return (perms & perm) != 0;
}

esp_err_t michi_pairing_revoke(const char *controller_id)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!id_valid(controller_id)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t slot = s_blob.count;
    for (size_t i = 0; i < s_blob.count; i++) {
        if (strcmp((const char *)s_blob.controllers[i].controller_id,
                   controller_id) == 0) {
            slot = i;
            break;
        }
    }
    if (slot >= s_blob.count) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    michi_pairing_blob_t next = s_blob;
    memmove(&next.controllers[slot], &next.controllers[slot + 1],
            (next.count - slot - 1) * sizeof(next.controllers[0]));
    next.count--;
    /* Zero the vacated slot: the revoked digest must not linger in RAM
     * (persist_blob also zeroes the tail for flash - F7c). */
    memset(&next.controllers[next.count], 0, sizeof(next.controllers[0]));

    const esp_err_t perr = persist_blob(&next);
    if (perr != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return perr;
    }
    s_blob = next;

    ESP_LOGI(TAG, "pairing: revoked controller=%s remaining=%u",
             controller_id, (unsigned)s_blob.count);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t michi_pairing_list(char *out, size_t out_len)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t pos = 0;
    out[0] = '\0';
    for (size_t i = 0; i < s_blob.count; i++) {
        const char *id = (const char *)s_blob.controllers[i].controller_id;
        const size_t id_len = strlen(id);
        const size_t need = id_len + (i + 1 < s_blob.count ? 2 : 1);
        if (pos + need > out_len) {
            xSemaphoreGive(s_mutex);
            return ESP_ERR_INVALID_SIZE;
        }
        if (i > 0) {
            out[pos++] = ',';
            out[pos++] = ' ';
        }
        memcpy(out + pos, id, id_len);
        pos += id_len;
        out[pos] = '\0';
    }
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t michi_pairing_close_window(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    window_close_locked("requested", true);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t michi_pairing_shutdown(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }
    /* Deleting a timer from inside its own callback is not supported:
     * shutdown must be called from regular task context.
     *
     * Teardown order (documented contract, F3):
     *   1. s_teardown = true FIRST: a window_timer_cb already dispatched
     *      checks it BEFORE touching the mutex and returns - the callback
     *      can never block on (or take) a mutex that is about to be
     *      deleted.
     *   2. esp_timer_stop: no new callback can fire from here on (stop on
     *      an already-fired one-shot is a no-op; the dispatched callback
     *      is neutralized by step 1).
     *   3. Take the mutex, delete the timer, release the mutex.
     *   4. ONLY THEN delete the mutex (vSemaphoreDelete) and clear
     *      s_initialized.
     * The callback never holds the mutex during teardown, so step 4 can
     * never race a pending take. */
    s_teardown = true;
    esp_timer_stop(s_timer);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* Silent close: the FSM bus may already be down; the close is still
     * logged (state=off below plus the window=closed line). */
    window_close_locked("shutdown", false);
    esp_timer_delete(s_timer);
    s_timer = NULL;
    xSemaphoreGive(s_mutex);
    vSemaphoreDelete(s_mutex);
    s_mutex = NULL;
    s_initialized = false;

    ESP_LOGI(TAG, "subsystem=pairing state=off phase=10");
    return ESP_OK;
}
