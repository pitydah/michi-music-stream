#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"

#include "mbedtls/sha256.h"

#include "michi_pairing.h"
#include "michi_state.h"
#include "validators.h"

#define TAG "michi_pairing"

#define MICHI_PAIRING_NVS_NAMESPACE "michi_pairing"
#define MICHI_PAIRING_NVS_KEY "controllers"
#define MICHI_PAIRING_BLOB_VERSION 2u

/* Rejection-sampling bound for a UNIFORM 6-digit PIN: 4294 * 1000000 is
 * the largest multiple of 1000000 not exceeding 2^32, so accepting
 * v < 4294000000 and taking v % 1000000 is uniform (no modulo bias). */
#define MICHI_PAIRING_PIN_UNIFORM_BOUND 4294000000u

/* Distinct source IPs remembered per window for the per-IP pair/start
 * rate limit. A window that sees more distinct IPs than this lets the
 * oldest slot be reclaimed (the global limit still holds). */
#define MICHI_PAIRING_IP_SLOTS 4u

/* One persisted controller. Field order is deliberate: the u32 pair
 * keeps 4-byte alignment, the int64 fields land 8-aligned with a fixed
 * 3-byte pad between them and the tail is exact (184 bytes). Entries are
 * zeroed before filling, so the pad bytes and the persisted bytes are
 * deterministic. _Static_assert guards the layout so the persisted blob
 * format cannot silently change. */
typedef struct {
    uint8_t device_id[MICHI_PAIRING_DEVICE_ID_LEN];      /* UUID v4 + NUL */
    uint8_t michi_id[MICHI_IDENTITY_MICHI_ID_LEN];       /* 43 + NUL */
    uint8_t public_key[MICHI_IDENTITY_PUBLIC_KEY_B64_LEN]; /* 43 + NUL */
    uint8_t digest[MICHI_PAIRING_DIGEST_BYTES];          /* SHA-256 of the token */
    uint32_t permissions;
    uint32_t reserved;                                   /* layout stability */
    int64_t created_unix;
    int64_t last_activity_unix;
} michi_controller_entry_t;

_Static_assert(sizeof(michi_controller_entry_t) == 184,
               "controller entry layout changed; the persisted blob format breaks");

typedef struct {
    uint32_t version;
    uint32_t count;
    michi_controller_entry_t controllers[CONFIG_MICHI_PAIRING_MAX_CONTROLLERS];
} michi_pairing_blob_t;

_Static_assert(sizeof(michi_pairing_blob_t) ==
                   8 + CONFIG_MICHI_PAIRING_MAX_CONTROLLERS * 184,
               "blob layout changed; the persisted blob format breaks");

/* One in-RAM pairing session. Dies with the window (re-open clears all);
 * survives the window close long enough for pair/status to report
 * "expired"/"confirmed"/"locked" until the next button press. */
typedef struct {
    bool in_use;
    michi_pairing_session_status_t status;
    uint8_t attempts_remaining;
    char session_id[MICHI_PAIRING_SESSION_ID_LEN];
    char pin[MICHI_PAIRING_PIN_BUF_LEN];
    char peer_michi_id[MICHI_IDENTITY_MICHI_ID_LEN];
    char peer_public_key[MICHI_IDENTITY_PUBLIC_KEY_B64_LEN];
    int64_t expires_mono_us;      /* window deadline (monotonic) */
    char expires_at[MICHI_PAIRING_EXPIRES_AT_LEN]; /* RFC 3339 */
} michi_pairing_session_t;

/* Per-IP pair/start rate limit slot (per window). */
typedef struct {
    char ip[MICHI_PAIRING_IP_MAX];
    uint32_t count;
} michi_pairing_ip_slot_t;

static SemaphoreHandle_t s_mutex;
static esp_timer_handle_t s_timer;
static volatile bool s_initialized;
/* Teardown flag (shutdown): window_timer_cb checks it BEFORE taking the
 * mutex - a callback already dispatched when shutdown runs must return
 * without touching the mutex (shutdown deletes it). Set before
 * esp_timer_stop, cleared by a later init. */
static volatile bool s_teardown;
static bool s_window_open;
/* Window opened at (esp_timer_get_time, us): monotonic reference and the
 * deadline check that keeps the getters honest during the tiny window
 * between the deadline and the timer callback. */
static int64_t s_window_opened_us;
static michi_pairing_session_t s_sessions[MICHI_PAIRING_MAX_SESSIONS_PER_WINDOW];
static uint32_t s_starts_per_window;
static michi_pairing_ip_slot_t s_ip_slots[MICHI_PAIRING_IP_SLOTS];
static michi_pairing_blob_t s_blob;
/* PIN display callback (single slot, registered at boot). Read under the
 * mutex, invoked OUTSIDE it (the callback may do I/O; it never calls
 * back into pairing). */
static michi_pairing_pin_display_cb_t s_pin_display_cb;
static void *s_pin_display_ctx;
/* Last wall time (s) the activity refresh was persisted (flash-wear
 * throttle for validate_token). */
static int64_t s_last_activity_persist;

/* --- small helpers ---------------------------------------------------- */

static int64_t now_unix(void)
{
    /* Wall clock (SNTP); 0 until a wall clock lands - same convention as
     * the discovery timestamp_ms. */
    return (int64_t)time(NULL);
}

/* RFC 3339 UTC "YYYY-MM-DDTHH:MM:SSZ". */
static void rfc3339_from_unix(int64_t unix, char *out, size_t out_len)
{
    if (out == NULL || out_len < MICHI_PAIRING_EXPIRES_AT_LEN) {
        return;
    }
    const time_t t = (time_t)unix;
    struct tm tm_buf;
    if (gmtime_r(&t, &tm_buf) == NULL) {
        snprintf(out, out_len, "1970-01-01T00:00:00Z");
        return;
    }
    /* Bounded fields (%u with explicit ranges): the output is exactly
     * 20 chars + NUL, the compiler can prove no truncation. */
    const unsigned y = (unsigned)(tm_buf.tm_year + 1900) % 10000u;
    const unsigned mo = (unsigned)(tm_buf.tm_mon + 1) % 100u;
    const unsigned d = (unsigned)tm_buf.tm_mday % 100u;
    const unsigned h = (unsigned)tm_buf.tm_hour % 100u;
    const unsigned mi = (unsigned)tm_buf.tm_min % 100u;
    const unsigned s = (unsigned)tm_buf.tm_sec % 100u;
    snprintf(out, out_len, "%04u-%02u-%02uT%02u:%02u:%02uZ", y, mo, d, h,
             mi, s);
}

static void uuid_v4_generate(char *out, size_t out_len)
{
    if (out == NULL || out_len < MICHI_PAIRING_SESSION_ID_LEN) {
        return;
    }
    const uint32_t a = esp_random();
    const uint32_t b = esp_random();
    const uint32_t c = esp_random();
    const uint32_t d = esp_random();
    /* UUID v4: time_low - time_mid - 4xxx - (10xx variant) - node,
     * lowercase hex, the canonical 36-char grouping
     * michi_pairing_uuid_valid accepts (8-4-4-4-12). */
    snprintf(out, out_len,
             "%08" PRIx32 "-%04x-4%03x-%04x-%012" PRIx32,
             a,
             (unsigned int)(b & 0xFFFFu),
             (unsigned int)((b >> 16) & 0xFFFu),
             (unsigned int)((c & 0x3FFFu) | 0x8000u),
             d);
}

/* Uniform 6-digit PIN (rejection sampling over esp_fill_random). */
static void pin_generate(char out[MICHI_PAIRING_PIN_BUF_LEN])
{
    uint8_t bytes[4];
    uint32_t v;
    do {
        esp_fill_random(bytes, sizeof(bytes));
        v = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
            ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    } while (v >= MICHI_PAIRING_PIN_UNIFORM_BOUND);
    snprintf(out, MICHI_PAIRING_PIN_BUF_LEN, "%06" PRIu32,
             v % 1000000u);
}

/* SHA-256 one-shot (mbedtls). */
static esp_err_t sha256_bytes(const uint8_t *data, size_t len, uint8_t *out)
{
    if (data == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mbedtls_sha256(data, len, out, 0) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* SHA-256 of a base64url-nopad token (43 chars, 32 bytes decoded). */
static esp_err_t digest_of_token(const char *token, uint8_t *out)
{
    if (token == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t raw[MICHI_PAIRING_TOKEN_BYTES];
    size_t raw_len = 0;
    if (michi_identity_base64url_decode(token, raw, sizeof(raw),
                                        &raw_len) != ESP_OK ||
        raw_len != MICHI_PAIRING_TOKEN_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    return sha256_bytes(raw, raw_len, out);
}

/* --- persistence ------------------------------------------------------ */

static esp_err_t persist_blob(const michi_pairing_blob_t *blob)
{
    /* Deterministic blob: zero the slots [count..MAX) before persisting
     * so a revoked/compacted entry's digest can never survive on flash.
     * Written to a local copy: the caller's blob is not mutated. */
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

/* Validate the loaded entries: every device_id must be a canonical UUID
 * string. An invalid entry is DROPPED (compacted + count--, warn logged)
 * instead of rejecting the whole store: one corrupt slot must not wipe
 * the registry. The cleanup is persisted by the next mutation. */
static void sanitize_loaded_blob(void)
{
    for (size_t i = 0; i < s_blob.count;) {
        const char *id = (const char *)s_blob.controllers[i].device_id;
        if (!michi_pairing_uuid_valid(id)) {
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
    /* Corrupt, truncated or foreign-version store (including the
     * phase-10 version-1 layout): start EMPTY instead of trusting it
     * (the next mutation overwrites it). */
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

static void sessions_clear_locked(void)
{
    memset(s_sessions, 0, sizeof(s_sessions));
    memset(s_ip_slots, 0, sizeof(s_ip_slots));
    s_starts_per_window = 0;
}

static void pin_display_notify(const char *pin)
{
    /* Snapshot the callback under the mutex, invoke OUTSIDE it. Only
     * called from task/timer context with the mutex free (never while
     * holding it). */
    michi_pairing_pin_display_cb_t cb = NULL;
    void *cb_ctx = NULL;
    if (!s_initialized) {
        cb = s_pin_display_cb;
        cb_ctx = s_pin_display_ctx;
    } else {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        cb = s_pin_display_cb;
        cb_ctx = s_pin_display_ctx;
        xSemaphoreGive(s_mutex);
    }
    if (cb != NULL) {
        cb(pin, cb_ctx);
    }
}

/* The single close path. notify=false: no FSM event (used by
 * open_window's silent re-open and by shutdown, where the bus may be
 * down); the close is always logged with the per-window counters. The
 * PIN display is cleared (NULL callback) on every close.
 *
 * NOTE: the close does NOT clear the pairing sessions: a session past
 * its deadline must stay answerable with status "expired" (contract
 * 2.3). Sessions die on the next button press (re-open replaces them)
 * and on reboot (init). */
static void window_close_locked(const char *reason, bool notify)
{
    if (!s_window_open) {
        return;
    }
    const uint32_t starts = s_starts_per_window;
    esp_timer_stop(s_timer);
    s_window_open = false;
    ESP_LOGI(TAG, "pairing: window=closed reason=%s starts=%u", reason,
             (unsigned)starts);
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
     * The window may have been re-opened after this timer fired: if the
     * deadline has not passed for the CURRENT window, this callback is
     * stale - it must NOT close the fresh window. */
    const int64_t deadline =
        s_window_opened_us +
        (int64_t)CONFIG_MICHI_PAIRING_WINDOW_SECONDS * 1000000;
    if (esp_timer_get_time() < deadline) {
        xSemaphoreGive(s_mutex);
        return;
    }
    window_close_locked("expired", true);
    xSemaphoreGive(s_mutex);
    /* Expiry clears the PIN screen too (the timer task is a regular
     * task context; the display callback never blocks). */
    pin_display_notify(NULL);
}

/* --- rate limiting ---------------------------------------------------- */

/* Every pair/start request inside the window counts (valid or not): a
 * brute-force spamming bad signatures is rate-limited the same way. */
static bool start_rate_limited_locked(const char *ip)
{
    if (s_starts_per_window >= MICHI_PAIRING_MAX_STARTS_PER_WINDOW) {
        ESP_LOGW(TAG, "pairing: window=active start_rate_limited scope=global");
        return true;
    }
    if (ip == NULL || ip[0] == '\0') {
        return false;
    }
    michi_pairing_ip_slot_t *free_slot = NULL;
    for (size_t i = 0; i < MICHI_PAIRING_IP_SLOTS; i++) {
        if (s_ip_slots[i].ip[0] == '\0') {
            if (free_slot == NULL) {
                free_slot = &s_ip_slots[i];
            }
            continue;
        }
        if (strcmp(s_ip_slots[i].ip, ip) == 0) {
            if (s_ip_slots[i].count >= MICHI_PAIRING_MAX_STARTS_PER_IP) {
                ESP_LOGW(TAG, "pairing: window=active start_rate_limited scope=ip");
                return true;
            }
            s_ip_slots[i].count++;
            s_starts_per_window++;
            return false;
        }
    }
    /* Unknown IP: claim a free slot (or reuse the oldest when full - the
     * global limit still bounds the window). */
    if (free_slot == NULL) {
        free_slot = &s_ip_slots[0];
    }
    strlcpy(free_slot->ip, ip, sizeof(free_slot->ip));
    free_slot->count = 1;
    s_starts_per_window++;
    return false;
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
    /* Belt and braces: the version field must ALWAYS be written, on
     * every load path, so the next persist survives the NVS round-trip
     * version check. A fresh boot has NO window and NO pairing sessions
     * (both are RAM-only). */
    s_blob.version = MICHI_PAIRING_BLOB_VERSION;
    sessions_clear_locked();
    s_window_open = false;
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
        /* Silent re-open: no FSM event (the button stays in PAIRING; the
         * window + sessions + counters are replaced). */
        window_close_locked("reopened", false);
    }
    /* Contract: "abrir de nuevo reemplaza la ventana previa y elimina
     * sesiones de pairing pendientes". */
    sessions_clear_locked();

    s_window_open = true;
    s_window_opened_us = esp_timer_get_time();

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
    /* The screen must clear any stale PIN from a previous window. */
    pin_display_notify(NULL);
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

michi_pairing_start_result_t michi_pairing_start(
    const michi_pairing_peer_t *peer, const char *ip,
    char *out_session_id, size_t session_id_len,
    char *out_expires_at, size_t expires_len,
    uint32_t *out_attempts_remaining)
{
    if (!s_initialized) {
        return MICHI_PAIRING_START_INTERNAL;
    }
    if (peer == NULL || out_session_id == NULL || out_expires_at == NULL ||
        out_attempts_remaining == NULL ||
        session_id_len < MICHI_PAIRING_SESSION_ID_LEN ||
        expires_len < MICHI_PAIRING_EXPIRES_AT_LEN) {
        return MICHI_PAIRING_START_INVALID;
    }

    /* Contract order: the physical window (403) and the rate limits (429)
     * gate the request BEFORE the peer validation. Every pair/start
     * request inside the window counts against the limits, valid or not
     * (a brute-force spamming bad signatures is limited the same way). */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!window_active_locked()) {
        xSemaphoreGive(s_mutex);
        return MICHI_PAIRING_START_WINDOW_CLOSED;
    }
    const bool rate_limited = start_rate_limited_locked(ip);
    xSemaphoreGive(s_mutex);
    if (rate_limited) {
        return MICHI_PAIRING_START_RATE_LIMITED;
    }

    uint8_t nonce_bytes[MICHI_PAIRING_NONCE_B64_MAX];
    uint8_t sig_bytes[MICHI_IDENTITY_SIGNATURE_BYTES];
    uint8_t pk_bytes[MICHI_IDENTITY_KEY_BYTES];
    char derived_id[MICHI_IDENTITY_MICHI_ID_LEN];
    size_t nonce_len = 0;
    size_t sig_len = 0;
    size_t pk_len = 0;

    /* Peer validation is stateless (michi_identity): the signature is
     * verified over the DECODED nonce bytes and michi_id must be the
     * canonical derivation of public_key. A failure creates NO session. */
    if (michi_identity_base64url_decode(peer->challenge_nonce, nonce_bytes,
                                        sizeof(nonce_bytes),
                                        &nonce_len) != ESP_OK ||
        michi_identity_base64url_decode(peer->challenge_signature, sig_bytes,
                                        sizeof(sig_bytes),
                                        &sig_len) != ESP_OK ||
        michi_identity_base64url_decode(peer->public_key, pk_bytes,
                                        sizeof(pk_bytes), &pk_len) != ESP_OK ||
        nonce_len < 16 || pk_len != MICHI_IDENTITY_KEY_BYTES ||
        sig_len != MICHI_IDENTITY_SIGNATURE_BYTES) {
        return MICHI_PAIRING_START_INVALID;
    }
    if (!michi_identity_verify(nonce_bytes, nonce_len, sig_bytes,
                               pk_bytes)) {
        ESP_LOGW(TAG, "pairing: start_rejected reason=signature");
        return MICHI_PAIRING_START_INVALID;
    }
    if (michi_identity_derive_michi_id(pk_bytes, derived_id,
                                       sizeof(derived_id)) != ESP_OK ||
        strcmp(derived_id, peer->michi_id) != 0) {
        /* michi_id is public: plain strcmp is fine. */
        ESP_LOGW(TAG, "pairing: start_rejected reason=michi_id");
        return MICHI_PAIRING_START_INVALID;
    }

    michi_pairing_session_t *slot = NULL;
    michi_pairing_pin_display_cb_t cb = NULL;
    void *cb_ctx = NULL;
    char pin[MICHI_PAIRING_PIN_BUF_LEN];

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* The window may have expired between the gate above and now (or a
     * concurrent close): re-check before creating the session. */
    if (!window_active_locked()) {
        xSemaphoreGive(s_mutex);
        return MICHI_PAIRING_START_WINDOW_CLOSED;
    }
    for (size_t i = 0; i < MICHI_PAIRING_MAX_SESSIONS_PER_WINDOW; i++) {
        if (!s_sessions[i].in_use) {
            slot = &s_sessions[i];
            break;
        }
    }
    if (slot == NULL) {
        ESP_LOGW(TAG, "pairing: window=active sessions_full");
        xSemaphoreGive(s_mutex);
        return MICHI_PAIRING_START_RATE_LIMITED;
    }

    /* Build the session: PIN and session_id are CSPRNG (uniform PIN via
     * rejection sampling). The deadline is the window deadline. */
    pin_generate(pin);
    uuid_v4_generate(slot->session_id, sizeof(slot->session_id));
    slot->status = MICHI_PAIRING_SESSION_PENDING;
    slot->attempts_remaining = MICHI_PAIRING_PIN_ATTEMPTS;
    slot->expires_mono_us =
        s_window_opened_us +
        (int64_t)CONFIG_MICHI_PAIRING_WINDOW_SECONDS * 1000000;
    rfc3339_from_unix(now_unix() + (int64_t)CONFIG_MICHI_PAIRING_WINDOW_SECONDS,
                      slot->expires_at, sizeof(slot->expires_at));
    strlcpy(slot->pin, pin, sizeof(slot->pin));
    strlcpy(slot->peer_michi_id, peer->michi_id,
            sizeof(slot->peer_michi_id));
    strlcpy(slot->peer_public_key, peer->public_key,
            sizeof(slot->peer_public_key));
    slot->in_use = true;
    cb = s_pin_display_cb;
    cb_ctx = s_pin_display_ctx;

    strlcpy(out_session_id, slot->session_id, session_id_len);
    strlcpy(out_expires_at, slot->expires_at, expires_len);
    *out_attempts_remaining = MICHI_PAIRING_PIN_ATTEMPTS;

    ESP_LOGI(TAG, "pairing: session=created session_id=%s",
             slot->session_id);
    xSemaphoreGive(s_mutex);

    /* Show the PIN locally (never over HTTP). The callback runs outside
     * the mutex. */
    if (cb != NULL) {
        cb(pin, cb_ctx);
    }
    return MICHI_PAIRING_START_OK;
}

/* Status name of a session, deadline-aware (a pending session past its
 * deadline reports "expired" without mutating the stored status). */
static const char *session_status_name_locked(
    const michi_pairing_session_t *s, int64_t now_us)
{
    if (s->status == MICHI_PAIRING_SESSION_PENDING &&
        now_us >= s->expires_mono_us) {
        return "expired";
    }
    switch (s->status) {
    case MICHI_PAIRING_SESSION_PENDING:   return "pending";
    case MICHI_PAIRING_SESSION_CONFIRMED: return "confirmed";
    case MICHI_PAIRING_SESSION_EXPIRED:   return "expired";
    case MICHI_PAIRING_SESSION_LOCKED:    return "locked";
    }
    return "pending";
}

michi_pairing_status_result_t michi_pairing_status(
    const char *session_id, char *out_status, size_t status_len,
    char *out_expires_at, size_t expires_len,
    uint32_t *out_attempts_remaining)
{
    if (!s_initialized) {
        return MICHI_PAIRING_STATUS_NOT_FOUND;
    }
    if (session_id == NULL || out_status == NULL || out_expires_at == NULL ||
        out_attempts_remaining == NULL || status_len == 0 ||
        expires_len < MICHI_PAIRING_EXPIRES_AT_LEN) {
        return MICHI_PAIRING_STATUS_NOT_FOUND;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const michi_pairing_session_t *s = NULL;
    for (size_t i = 0; i < MICHI_PAIRING_MAX_SESSIONS_PER_WINDOW; i++) {
        if (s_sessions[i].in_use &&
            strcmp(s_sessions[i].session_id, session_id) == 0) {
            s = &s_sessions[i];
            break;
        }
    }
    if (s == NULL) {
        xSemaphoreGive(s_mutex);
        return MICHI_PAIRING_STATUS_NOT_FOUND;
    }
    strlcpy(out_status,
            session_status_name_locked(s, esp_timer_get_time()), status_len);
    strlcpy(out_expires_at, s->expires_at, expires_len);
    *out_attempts_remaining = s->attempts_remaining;
    xSemaphoreGive(s_mutex);
    return MICHI_PAIRING_STATUS_OK;
}

michi_pairing_confirm_result_t michi_pairing_confirm(
    const char *session_id, const char *pin, const char *michi_id,
    const char *public_key, char *out_token, size_t token_len,
    char *out_device_id, size_t device_id_len)
{
    if (!s_initialized) {
        return MICHI_PAIRING_CONFIRM_INTERNAL;
    }
    if (session_id == NULL || pin == NULL || michi_id == NULL ||
        public_key == NULL || out_token == NULL || out_device_id == NULL ||
        token_len < MICHI_PAIRING_TOKEN_B64_LEN ||
        device_id_len < MICHI_PAIRING_DEVICE_ID_LEN) {
        return MICHI_PAIRING_CONFIRM_INVALID;
    }
    if (!michi_pairing_pin_valid(pin)) {
        /* Malformed PIN: rejected before it can consume an attempt. */
        return MICHI_PAIRING_CONFIRM_INVALID;
    }

    uint8_t token_raw[MICHI_PAIRING_TOKEN_BYTES];
    uint8_t digest[MICHI_PAIRING_DIGEST_BYTES];
    char device_id[MICHI_PAIRING_DEVICE_ID_LEN];

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    michi_pairing_session_t *s = NULL;
    for (size_t i = 0; i < MICHI_PAIRING_MAX_SESSIONS_PER_WINDOW; i++) {
        if (s_sessions[i].in_use &&
            strcmp(s_sessions[i].session_id, session_id) == 0) {
            s = &s_sessions[i];
            break;
        }
    }
    if (s == NULL) {
        xSemaphoreGive(s_mutex);
        return MICHI_PAIRING_CONFIRM_NOT_FOUND;
    }
    if (s->status == MICHI_PAIRING_SESSION_CONFIRMED) {
        /* Second confirmation: the token was issued once, the session is
         * consumed. */
        ESP_LOGW(TAG, "pairing: confirm_rejected reason=already_confirmed");
        xSemaphoreGive(s_mutex);
        return MICHI_PAIRING_CONFIRM_CONFLICT;
    }
    if (s->status == MICHI_PAIRING_SESSION_LOCKED) {
        /* Sixth attempt and beyond: 429, the session stays consumed. */
        ESP_LOGW(TAG, "pairing: confirm_rejected reason=locked");
        xSemaphoreGive(s_mutex);
        return MICHI_PAIRING_CONFIRM_LOCKED;
    }
    if (esp_timer_get_time() >= s->expires_mono_us) {
        xSemaphoreGive(s_mutex);
        return MICHI_PAIRING_CONFIRM_NOT_FOUND;
    }
    /* Identity must be EXACTLY the pair/start one. michi_id/public_key
     * are public: plain strcmp is fine. */
    if (strcmp(s->peer_michi_id, michi_id) != 0 ||
        strcmp(s->peer_public_key, public_key) != 0) {
        ESP_LOGW(TAG, "pairing: confirm_rejected reason=identity_mismatch");
        xSemaphoreGive(s_mutex);
        return MICHI_PAIRING_CONFIRM_INVALID;
    }
    /* Constant-time PIN check: never reveal the comparison result by
     * timing. */
    if (!michi_pairing_token_matches((const uint8_t *)pin,
                                     (const uint8_t *)s->pin,
                                     MICHI_PAIRING_PIN_LEN)) {
        if (s->attempts_remaining > 0) {
            s->attempts_remaining--;
        }
        if (s->attempts_remaining == 0) {
            /* Five failed attempts: the session is consumed (locked); the
             * NEXT confirm answers 429. */
            s->status = MICHI_PAIRING_SESSION_LOCKED;
            ESP_LOGW(TAG, "pairing: session=locked session_id=%s",
                     s->session_id);
        }
        ESP_LOGW(TAG, "pairing: confirm_rejected reason=pin attempts_left=%u",
                 (unsigned)s->attempts_remaining);
        xSemaphoreGive(s_mutex);
        return MICHI_PAIRING_CONFIRM_PIN_MISMATCH;
    }

    /* Correct PIN: mint the token (32 CSPRNG bytes -> base64url-nopad),
     * the device_id, and persist ONLY the digest before anything is
     * returned. */
    esp_fill_random(token_raw, sizeof(token_raw));
    if (michi_identity_base64url_encode(token_raw, sizeof(token_raw),
                                        out_token, token_len) != ESP_OK ||
        sha256_bytes(token_raw, sizeof(token_raw), digest) != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return MICHI_PAIRING_CONFIRM_INTERNAL;
    }
    uuid_v4_generate(device_id, sizeof(device_id));

    if (s_blob.count >= CONFIG_MICHI_PAIRING_MAX_CONTROLLERS) {
        ESP_LOGW(TAG, "pairing: registry_full controllers=%u",
                 (unsigned)s_blob.count);
        xSemaphoreGive(s_mutex);
        return MICHI_PAIRING_CONFIRM_INTERNAL;
    }

    /* Build the next state in a local copy, persist it, THEN apply it: a
     * failed NVS write never leaves the in-RAM registry half-mutated and
     * never issues a token that would not survive a reboot. */
    michi_pairing_blob_t next = s_blob;
    michi_controller_entry_t *e = &next.controllers[next.count];
    memset(e, 0, sizeof(*e));
    strlcpy((char *)e->device_id, device_id, sizeof(e->device_id));
    strlcpy((char *)e->michi_id, michi_id, sizeof(e->michi_id));
    strlcpy((char *)e->public_key, public_key, sizeof(e->public_key));
    memcpy(e->digest, digest, sizeof(digest));
    e->permissions = MICHI_PERM_DEFAULT;
    e->created_unix = now_unix();
    e->last_activity_unix = e->created_unix;
    next.count++;

    const esp_err_t perr = persist_blob(&next);
    if (perr != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return MICHI_PAIRING_CONFIRM_INTERNAL;
    }
    s_blob = next;
    s->status = MICHI_PAIRING_SESSION_CONFIRMED;

    strlcpy(out_device_id, device_id, device_id_len);
    ESP_LOGI(TAG, "pairing: confirmed session_id=%s device_id=%s michi_id=%s",
             s->session_id, device_id, michi_id);
    xSemaphoreGive(s_mutex);
    /* A confirmed session must not leave its PIN on the screen. */
    pin_display_notify(NULL);
    return MICHI_PAIRING_CONFIRM_OK;
}

esp_err_t michi_pairing_validate_token(const char *token,
                                       char *out_device_id,
                                       size_t id_len,
                                       uint32_t *out_permissions)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (token == NULL || out_device_id == NULL ||
        out_permissions == NULL ||
        id_len < MICHI_PAIRING_DEVICE_ID_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t digest[MICHI_PAIRING_DIGEST_BYTES];
    const esp_err_t derr = digest_of_token(token, digest);
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
        if (michi_pairing_token_matches(ref, digest, sizeof(digest))) {
            matched = (int)i;
        }
    }
    if (matched < 0 || (size_t)matched >= s_blob.count) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    strlcpy(out_device_id,
            (const char *)s_blob.controllers[matched].device_id, id_len);
    *out_permissions = s_blob.controllers[matched].permissions;

    /* Last-activity refresh: RAM always, NVS at most once per
     * MICHI_PAIRING_ACTIVITY_PERSIST_SECONDS (flash-wear guard). A
     * failed refresh is logged but never fails the validation. */
    s_blob.controllers[matched].last_activity_unix = now_unix();
    if (now_unix() - s_last_activity_persist >=
        MICHI_PAIRING_ACTIVITY_PERSIST_SECONDS) {
        if (persist_blob(&s_blob) == ESP_OK) {
            s_last_activity_persist = now_unix();
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

bool michi_pairing_has_permission(const char *token, uint32_t perm)
{
    uint32_t perms = 0;
    char id[MICHI_PAIRING_DEVICE_ID_LEN];
    /* Any validation failure (malformed, unknown, before init) is
     * "no permission": the convenience wrapper does not distinguish
     * invalid from forbidden. */
    if (michi_pairing_validate_token(token, id, sizeof(id), &perms) !=
        ESP_OK) {
        return false;
    }
    return (perms & perm) != 0;
}

esp_err_t michi_pairing_revoke(const char *device_id)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!michi_pairing_uuid_valid(device_id)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t slot = s_blob.count;
    for (size_t i = 0; i < s_blob.count; i++) {
        if (strcmp((const char *)s_blob.controllers[i].device_id,
                   device_id) == 0) {
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
     * (persist_blob also zeroes the tail for flash). */
    memset(&next.controllers[next.count], 0, sizeof(next.controllers[0]));

    const esp_err_t perr = persist_blob(&next);
    if (perr != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return perr;
    }
    s_blob = next;

    ESP_LOGI(TAG, "pairing: revoked device_id=%s remaining=%u",
             device_id, (unsigned)s_blob.count);
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
        const char *id = (const char *)s_blob.controllers[i].device_id;
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

esp_err_t michi_pairing_erase_all(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(MICHI_PAIRING_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pairing: erase_open_failed err=%s",
                 esp_err_to_name(err));
        return err;
    }
    err = nvs_erase_key(h, MICHI_PAIRING_NVS_KEY);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pairing: erase_failed err=%s", esp_err_to_name(err));
        return err;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(&s_blob, 0, sizeof(s_blob));
    s_blob.version = MICHI_PAIRING_BLOB_VERSION;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "pairing: erased controllers=0");
    return ESP_OK;
}

void michi_pairing_set_pin_display_cb(michi_pairing_pin_display_cb_t cb,
                                      void *ctx)
{
    if (!s_initialized) {
        s_pin_display_cb = cb;
        s_pin_display_ctx = ctx;
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_pin_display_cb = cb;
    s_pin_display_ctx = ctx;
    xSemaphoreGive(s_mutex);
}

esp_err_t michi_pairing_close_window(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    window_close_locked("requested", true);
    xSemaphoreGive(s_mutex);
    pin_display_notify(NULL);
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
    /* A reboot closes the window: the screen must not keep the PIN. The
     * display may already be down - the callback degrades gracefully. */
    pin_display_notify(NULL);
    return ESP_OK;
}
