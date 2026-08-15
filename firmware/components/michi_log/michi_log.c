/*
 * Hybrid log registry (phase 16): volatile tail in PSRAM + durable event
 * journal in SPIFFS + HTTP /logs endpoint (handler in michi_http).
 *
 * Layer 1 (tail): esp_log_set_vprintf override. IDF renders every ESP_LOG*
 * line through LOG_FORMAT(letter, format) =
 *   [color]<letter> " (%" PRIu32 ") %s: " format [reset] "\n"
 * with (timestamp, tag) as the first va_args - so the callback receives
 * the level letter in the format string and the rendered text carries the
 * tag. We render into a stack buffer, parse the prefix, and store
 * [uptime_ms u32][level u8][len u16][payload] in a PSRAM ring; the
 * console output is preserved by chaining the PREVIOUS vprintf (returned
 * by esp_log_set_vprintf). The callback NEVER logs, NEVER mallocs, holds
 * a short spinlock and drops instead of failing.
 *
 * Layer 2 (journal): the FSM observer (filter 0) forwards only
 * STATE_CHANGED / ERROR / UPDATE_FAILED to a queue; the journal task
 * appends one text line per event to /spiffs/logs/journal.1 and rotates
 * the rename chain when the active file exceeds the size limit. Cost of
 * open/append/close per event is intentional: tens of events per day on
 * SPIFFS (documented in firmware/README.md).
 *
 * Layer 3 (endpoint): implemented in michi_http (logs_get_handler).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "michi_log.h"
#include "michi_state.h"

#define TAG "michi_log"

/* Magic of the PSRAM ring header: survives soft restarts in PSRAM; a
 * mismatch (fresh power, different build, PSRAM not first allocation)
 * skips the crash dump. */
#define MICHI_LOG_RING_MAGIC 0x4D4C5247u /* "MLRG" */

/* Slot geometry: 8-byte entry header + 504 payload = 512 (natural
 * alignment). ESP_LOG lines are typically < 300 bytes; longer payloads
 * are truncated in the tail only. */
#define MICHI_LOG_ENTRY_PAYLOAD_MAX 504
#define MICHI_LOG_ENTRY_SLOT_BYTES 512

#define MICHI_LOG_NVS_NS "michi_log"
#define MICHI_LOG_NVS_BOOT_SEQ_KEY "boot_seq"

#define MICHI_LOG_SPIFFS_BASE "/spiffs"
#define MICHI_LOG_SPIFFS_PARTITION "storage"
#define MICHI_LOG_LOGS_DIR MICHI_LOG_SPIFFS_BASE "/logs"
#define MICHI_LOG_JOURNAL_FILE_FMT MICHI_LOG_LOGS_DIR "/journal.%d"
#define MICHI_LOG_CRASH_FILE_FMT MICHI_LOG_LOGS_DIR "/crash_%u.txt"
#define MICHI_LOG_CRASH_PREFIX_LEN 64

#define MICHI_LOG_TASK_PRIORITY 3

#define MICHI_LOG_ITEM_QUIT 0xFFFFFFFFu

typedef struct {
    uint32_t magic;     /* MICHI_LOG_RING_MAGIC */
    uint32_t slot_size; /* sizeof(michi_log_entry_t): build fingerprint */
    uint32_t slots;     /* ring capacity in slots */
    uint32_t head;      /* next write index (mod slots) */
    uint32_t count;     /* stored entries (<= slots) */
} michi_log_ring_hdr_t;

typedef struct {
    uint32_t t_ms;
    uint16_t len;   /* payload length incl NUL, 1..MICHI_LOG_ENTRY_PAYLOAD_MAX */
    uint8_t level;  /* 'I' 'W' 'E' 'D' 'V' or '?' (non-esp_log format) */
    uint8_t pad;
    char payload[MICHI_LOG_ENTRY_PAYLOAD_MAX];
} michi_log_entry_t;

_Static_assert(sizeof(michi_log_entry_t) == MICHI_LOG_ENTRY_SLOT_BYTES,
               "log entry must fit the fixed slot size");

/* M11: the t_ms field offset must stay coherent with the tail line
 * layout ("<level> <t_ms:010u> ": level at 0, space at 1, the 10-digit
 * field at 2 = PREFIX_LEN(13) - 11). */
_Static_assert(MICHI_LOG_TAIL_T_OFFSET == MICHI_LOG_TAIL_PREFIX_LEN - 11,
               "t_ms field offset must match the tail line layout");

typedef struct {
    uint32_t kind;      /* MICHI_LOG_ITEM_QUIT or an event id */
    uint32_t uptime_ms; /* esp_timer_get_time()/1000 at dispatch time */
    uint32_t data;
    uint32_t from;
} michi_log_journal_item_t;

static vprintf_like_t s_prev_vprintf = NULL;
static portMUX_TYPE s_tail_mux = portMUX_INITIALIZER_UNLOCKED;
static michi_log_ring_hdr_t *s_tail_ring = NULL;

static QueueHandle_t s_journal_queue = NULL;
static TaskHandle_t s_journal_task = NULL;
static uint32_t s_boot_seq = 0;
static volatile uint32_t s_dropped_events = 0;
static bool s_journal_mounted = false;
static bool s_observer_ok = false;

/* Journal task lifecycle (F3): the shutdown caller stores ITS task handle
 * here under the mux before enqueuing QUIT; the task notifies it (never
 * itself) after draining and exiting. If the queue is full the QUIT is
 * replaced by the s_journal_quit flag + a direct notify - the task polls
 * the flag with a bounded receive timeout, so the flag alone suffices. */
static portMUX_TYPE s_journal_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_shutdown_done = NULL;
static volatile bool s_journal_quit = false;

/* Write-failure latch (F5): once a journal write fails, no further write
 * is attempted until the next start_journal() (a degraded SPIFFS would
 * only produce repeated failing syscalls). The warning is emitted once. */
static bool s_journal_degraded = false;

/* Crash dump staging: the last MICHI_LOG_CRASH_DUMP_KB of the PREVIOUS
 * boot's ring, copied at init (before the ring is overwritten) and
 * flushed to SPIFFS by start_journal() - NVS/SPIFFS do not exist yet at
 * init time. */
static char *s_crash_staging = NULL;
static size_t s_crash_staging_len = 0;

/* ------------------------------------------------------------------
 * Tail: ring buffer in PSRAM
 * ------------------------------------------------------------------ */

static const char *split_rendered(const char *line, size_t len, uint8_t *level)
{
    size_t i = 0;
    if (i < len && line[i] == '\033') { /* ANSI color prefix */
        while (i < len && line[i] != 'm') {
            i++;
        }
        if (i < len) {
            i++;
        }
    }
    if (i >= len) {
        *level = '?';
        return line;
    }
    *level = (uint8_t)line[i];
    /* IDF LOG_FORMAT: <letter> " (%" PRIu32 ") %s: " ... */
    if (i + 2 < len && line[i + 1] == ' ' && line[i + 2] == '(') {
        size_t j = i + 3;
        while (j < len && line[j] >= '0' && line[j] <= '9') {
            j++;
        }
        if (j + 1 < len && line[j] == ')' && line[j + 1] == ' ') {
            return &line[j + 2];
        }
    }
    *level = '?';
    return line;
}

static size_t strip_rendered_suffix(const char *payload, size_t len)
{
    while (len > 0 && (payload[len - 1] == '\n' || payload[len - 1] == '\r')) {
        len--;
    }
    if (len >= 4 && memcmp(&payload[len - 4], "\033[0m", 4) == 0) {
        len -= 4;
    }
    return len;
}

/* Sanitize the stored payload: newlines would break the one-line-per-entry
 * contract of the tail endpoint, so any embedded \r or \n becomes a space.
 * Only the RING copy is sanitized - the console keeps the original. */
static void sanitize_payload(char *payload, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (payload[i] == '\n' || payload[i] == '\r') {
            payload[i] = ' ';
        }
    }
}

/* vprintf override: called by esp_log_writev from task context. MUST be
 * re-entrant (esp_log releases its lock before calling), MUST NOT log,
 * MUST NOT malloc. On any failure the entry is dropped - console output
 * via the previous vprintf is always chained. */
static int log_vprintf(const char *fmt, va_list args)
{
    /* M10: va_copy for correctness: the va_list state after vsnprintf is
     * unspecified per C11 7.16.1.1; behavior differs across ABIs (a
     * va_list-by-value ABI would leave the chained s_prev_vprintf
     * reading garbage). Copy once, consume the copy. */
    va_list ap2;
    va_copy(ap2, args);
    char buf[MICHI_LOG_ENTRY_PAYLOAD_MAX];
    const int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n > 0 && s_tail_ring != NULL &&
        s_tail_ring->magic == MICHI_LOG_RING_MAGIC) {
        /* vsnprintf returns what it WOULD have written; when the line is
         * truncated (n >= sizeof(buf)) only sizeof(buf)-1 bytes are
         * real - split_rendered must walk the real length, not the
         * would-have-been one. */
        const size_t real = ((size_t)n < sizeof(buf)) ? (size_t)n
                                                      : sizeof(buf) - 1;
        uint8_t level = '?';
        const char *payload = split_rendered(buf, real, &level);
        const size_t plen = strip_rendered_suffix(payload, strlen(payload));
        if (plen > 0) {
            michi_log_entry_t *entry = (michi_log_entry_t *)((uint8_t *)s_tail_ring +
                                                              sizeof(*s_tail_ring));
            portENTER_CRITICAL(&s_tail_mux);
            michi_log_entry_t *slot = &entry[s_tail_ring->head];
            slot->t_ms = (uint32_t)(esp_timer_get_time() / 1000);
            slot->level = level;
            slot->len = (uint16_t)(plen + 1);
            slot->pad = 0;
            memcpy(slot->payload, payload, plen);
            slot->payload[plen] = '\0';
            sanitize_payload(slot->payload, plen);
            s_tail_ring->head = (s_tail_ring->head + 1) % s_tail_ring->slots;
            if (s_tail_ring->count < s_tail_ring->slots) {
                s_tail_ring->count++;
            }
            portEXIT_CRITICAL(&s_tail_mux);
        }
    }
    if (s_prev_vprintf != NULL) {
        const int r = s_prev_vprintf(fmt, ap2);
        va_end(ap2);
        return r;
    }
    va_end(ap2);
    return n;
}

static void crash_stage(const michi_log_ring_hdr_t *prev)
{
    if (prev == NULL || prev->magic != MICHI_LOG_RING_MAGIC ||
        prev->slot_size != sizeof(michi_log_entry_t)) {
        ESP_LOGI(TAG, "crash dump: no valid previous ring at %p (skip)",
                 (void *)s_tail_ring);
        return;
    }
    /* The header of the previous boot is memory we do NOT trust: a crash
     * (or a stale PSRAM area) may hold arbitrary values. Validate EVERY
     * geometry field before using it - slots must fit the current boot's
     * ring, head must be in range, count must be sane. */
    const uint32_t cur_slots =
        (uint32_t)(((size_t)CONFIG_MICHI_LOG_TAIL_SIZE_KB * 1024u -
                    sizeof(*s_tail_ring)) /
                   MICHI_LOG_ENTRY_SLOT_BYTES);
    const uint32_t slots = prev->slots;
    const uint32_t head = prev->head;
    const uint32_t count = prev->count;
    if (slots < 1 || slots > cur_slots || head >= slots || count > slots ||
        count == 0) {
        ESP_LOGI(TAG, "crash dump: no valid previous ring at %p (skip)",
                 (void *)s_tail_ring);
        return;
    }
    const uint32_t dump_kb = CONFIG_MICHI_LOG_CRASH_DUMP_KB;
    const size_t cap = (size_t)dump_kb * 1024u;
    char *staging = heap_caps_malloc(cap + MICHI_LOG_CRASH_PREFIX_LEN,
                                     MALLOC_CAP_SPIRAM);
    if (staging == NULL) {
        ESP_LOGE(TAG, "crash dump: staging alloc failed (%u KB), dump lost",
                 (unsigned)dump_kb);
        return;
    }
    michi_log_entry_t *entry = (michi_log_entry_t *)((uint8_t *)s_tail_ring +
                                                      sizeof(*s_tail_ring));
    size_t used = 0;
    portENTER_CRITICAL(&s_tail_mux);
    uint32_t kept = count;
    if ((size_t)kept * MICHI_LOG_ENTRY_SLOT_BYTES > cap) {
        kept = (uint32_t)(cap / MICHI_LOG_ENTRY_SLOT_BYTES);
        if (kept == 0) {
            kept = 1;
        }
    }
    /* The previous boot's entries are NOT trusted: a crash can have
     * interrupted a ring write, so the payload is copied to a local
     * NUL-terminated buffer (bounded by the declared len) before it is
     * ever printed. */
    char payload[MICHI_LOG_ENTRY_PAYLOAD_MAX];
    /* Render the NEWEST entries FIRST and stop when the dump budget is
     * exhausted: the lines just before the crash are the most valuable
     * and MUST survive a truncation (a forward pass with a break on
     * overflow cuts exactly those). Walk backwards from head-1 to find
     * the oldest entry that still fits, then render that window in
     * chronological order. */
    uint32_t win_start = head; /* oldest entry of the window */
    uint32_t win_count = 0;
    size_t budget = cap;
    for (uint32_t i = 1; i <= kept; i++) {
        const michi_log_entry_t *e = &entry[(head + slots - i) % slots];
        const size_t plen = (e->len > 0 && e->len <= MICHI_LOG_ENTRY_PAYLOAD_MAX)
                                ? (size_t)e->len - 1
                                : 0;
        if (budget < MICHI_LOG_TAIL_PREFIX_LEN + 1 + plen) {
            /* M9: a rendered line is MICHI_LOG_TAIL_LINE_FMT
             * ("%c %010u %s\n") = MICHI_LOG_TAIL_PREFIX_LEN prefix
             * (level + space + 10-digit t_ms + space) + payload +
             * '\n' (1 byte). */
            break;
        }
        budget -= MICHI_LOG_TAIL_PREFIX_LEN + 1 + plen;
        win_start = (head + slots - i) % slots;
        win_count++;
    }
    for (uint32_t i = 0; i < win_count; i++) {
        const michi_log_entry_t *e = &entry[(win_start + i) % slots];
        const size_t plen = (e->len > 0 && e->len <= MICHI_LOG_ENTRY_PAYLOAD_MAX)
                                ? (size_t)e->len - 1
                                : 0;
        memcpy(payload, e->payload, plen);
        payload[plen] = '\0';
        int w = snprintf(staging + used, cap + MICHI_LOG_CRASH_PREFIX_LEN - used,
                         MICHI_LOG_TAIL_LINE_FMT,
                         (int)e->level, e->t_ms, payload);
        if (w < 0 || (size_t)w >= cap + MICHI_LOG_CRASH_PREFIX_LEN - used) {
            break; /* defensive: cannot happen by construction of the window */
        }
        used += (size_t)w;
    }
    portEXIT_CRITICAL(&s_tail_mux);
    if (used == 0) {
        heap_caps_free(staging);
        ESP_LOGW(TAG, "crash dump: dump abandoned: no storable entries");
        return;
    }
    s_crash_staging = staging;
    s_crash_staging_len = used;
    ESP_LOGW(TAG, "crash dump staged: %u entries, %u bytes (reset=%d)",
             (unsigned)win_count, (unsigned)used, (int)esp_reset_reason());
}

static bool reset_is_crash(void)
{
    const esp_reset_reason_t r = esp_reset_reason();
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
           r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
}

/* ------------------------------------------------------------------
 * Journal: SPIFFS + event queue
 * ------------------------------------------------------------------ */

static void journal_path(char *out, size_t out_len, int idx)
{
    snprintf(out, out_len, MICHI_LOG_JOURNAL_FILE_FMT, idx);
}

static uint32_t journal_max_bytes(void)
{
    return (uint32_t)CONFIG_MICHI_LOG_JOURNAL_MAX_KB * 1024u;
}

static void journal_rotate(void)
{
    const int files = CONFIG_MICHI_LOG_JOURNAL_FILES;
    if (files == 1) {
        /* Single-file mode: rotation IS truncation. journal_append
         * already closed the active file, so "w" is safe. */
        char path[64];
        journal_path(path, sizeof(path), 1);
        FILE *f = fopen(path, "w");
        if (f == NULL) {
            ESP_LOGW(TAG, "journal rotate: truncate %s failed", path);
        } else {
            fclose(f);
        }
        return;
    }
    for (int i = files; i >= 2; i--) {
        char dst[64];
        char src[64];
        journal_path(dst, sizeof(dst), i);
        journal_path(src, sizeof(src), i - 1);
        unlink(dst);
        if (rename(src, dst) != 0) {
            ESP_LOGW(TAG, "journal rotate: rename %s -> %s failed",
                     src, dst);
        }
    }
}

static void journal_mark_degraded(size_t failed_bytes)
{
    if (!s_journal_degraded) { /* single warning per run (F5) */
        ESP_LOGW(TAG, "journal: write_failed bytes=%u (journal degraded, "
                      "events not written until restart)",
                 (unsigned)failed_bytes);
    }
    s_journal_degraded = true;
}

static void journal_append(const michi_log_journal_item_t *item)
{
    char line[192];
    if (item->kind == MICHI_EVENT_STATE_CHANGED) {
        snprintf(line, sizeof(line),
                 "%u %u STATE_CHANGED target=%s from=%s\n",
                 (unsigned)s_boot_seq, (unsigned)item->uptime_ms,
                 michi_state_name((michi_state_t)item->data),
                 michi_state_name((michi_state_t)item->from));
    } else if (item->kind == MICHI_EVENT_ERROR ||
               item->kind == MICHI_EVENT_UPDATE_FAILED) {
        snprintf(line, sizeof(line), "%u %u %s err=%s\n",
                 (unsigned)s_boot_seq, (unsigned)item->uptime_ms,
                 item->kind == MICHI_EVENT_ERROR ? "ERROR" : "UPDATE_FAILED",
                 esp_err_to_name((esp_err_t)item->data));
    } else {
        return; /* filtered events only - defensive */
    }

    char path[64];
    journal_path(path, sizeof(path), 1);
    FILE *f = fopen(path, "a");
    if (f == NULL) {
        ESP_LOGW(TAG, "journal: open %s failed (event dropped)", path);
        return;
    }
    /* "a" mode starts at end-of-file (C11 7.21.5.3), so ftell() gives the
     * current size directly - no fseek needed (cppcheck seekOnAppendedFile). */
    const long sz = ftell(f);
    if (sz >= (long)journal_max_bytes()) {
        fclose(f);
        journal_rotate();
        f = fopen(path, "a");
        if (f == NULL) {
            ESP_LOGW(TAG, "journal: reopen after rotate failed");
            return;
        }
    }
    const size_t expect = strlen(line);
    /* F5/M1: verify the write AND the close - SPIFFS buffers, so an
     * error may surface only at fclose(). Both are evaluated separately
     * so the close ALWAYS runs: an early-return on write failure would
     * leak the fd and exhaust the SPIFFS max_files=8 pool. On failure
     * the journal latches degraded and stops trying until the next
     * start_journal(). */
    const size_t written = fwrite(line, 1, expect, f);
    const int rc = fclose(f);
    if (written != expect || rc != 0) {
        journal_mark_degraded(expect);
    }
}

static void journal_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* F3: the quit flag can be set without a queue item (queue full
         * during shutdown); poll it with a bounded receive timeout. The
         * 200 ms cadence is irrelevant at journal rates (tens/day). */
        if (s_journal_quit) {
            break;
        }
        michi_log_journal_item_t item;
        if (xQueueReceive(s_journal_queue, &item, pdMS_TO_TICKS(200)) !=
            pdTRUE) {
            continue;
        }
        if (item.kind == MICHI_LOG_ITEM_QUIT) {
            break;
        }
        if (!s_journal_mounted || s_journal_degraded) {
            continue;
        }
        journal_append(&item);
    }
    /* F3: notify the SHUTDOWN CALLER's handle (saved under the mux),
     * never our own; take the handle so the handshake is one-shot. */
    portENTER_CRITICAL(&s_journal_mux);
    TaskHandle_t done = s_shutdown_done;
    s_shutdown_done = NULL;
    portEXIT_CRITICAL(&s_journal_mux);
    if (done != NULL) {
        xTaskNotifyGive(done);
    }
    vTaskDelete(NULL);
}

/* FSM observer (runs on the event path: MUST NOT block). Non-blocking
 * queue send: a full queue drops the event and counts it. */
static void log_state_observer(const michi_event_t *ev)
{
    if (ev->id != MICHI_EVENT_STATE_CHANGED && ev->id != MICHI_EVENT_ERROR &&
        ev->id != MICHI_EVENT_UPDATE_FAILED) {
        return;
    }
    if (s_journal_queue == NULL) {
        return;
    }
    michi_log_journal_item_t item = {
        .kind = (uint32_t)ev->id,
        .uptime_ms = (uint32_t)(esp_timer_get_time() / 1000),
        .data = ev->data,
        .from = ev->from,
    };
    if (xQueueSend(s_journal_queue, &item, 0) != pdTRUE) {
        s_dropped_events++;
        ESP_LOGW(TAG, "journal queue full: %s event dropped (total=%u)",
                 ev->id == MICHI_EVENT_STATE_CHANGED ? "STATE_CHANGED"
                 : ev->id == MICHI_EVENT_ERROR ? "ERROR" : "UPDATE_FAILED",
                 (unsigned)s_dropped_events);
    }
}

/* Prune crash_<seq>.txt files beyond MICHI_LOG_CRASH_DUMP_KEEP: the seq
 * in the name is the boot_seq of the crashed boot, monotonically
 * increasing - the highest seq is the newest dump. Keep the NEWEST K,
 * delete the oldest extras.
 *
 * Convergence (M2): the scan cap MAX_DUMPS=64 exceeds any realistic dump
 * count (KEEP ranges 1..16), but a directory with MORE than 64 canonical
 * dumps is not fully converged in ONE pass - the next dump's prune picks
 * up the remainder (every crash dump write triggers a prune), so the
 * count converges to <= KEEP over successive writes. The cap bounds the
 * sort; it never unbounds storage. */
static void journal_prune_crash_dumps(void)
{
    enum { MAX_DUMPS = 64 };
    /* M2c: the found[][]/seqs arrays (64 x 80 + 64 x 4 = 5376 B) run on
     * the app_main stack (3584 B) via the flush path - heap-allocate
     * (PSRAM first, internal RAM fallback, freed at the end) instead. */
    const int keep = CONFIG_MICHI_LOG_CRASH_DUMP_KEEP;
    char (*found)[80] = NULL;
    uint32_t *seqs = NULL;
    found = heap_caps_malloc((size_t)MAX_DUMPS * 80, MALLOC_CAP_SPIRAM);
    if (found == NULL) {
        found = malloc((size_t)MAX_DUMPS * 80);
    }
    seqs = heap_caps_malloc((size_t)MAX_DUMPS * sizeof(uint32_t),
                            MALLOC_CAP_SPIRAM);
    if (seqs == NULL) {
        seqs = malloc((size_t)MAX_DUMPS * sizeof(uint32_t));
    }
    if (found == NULL || seqs == NULL) {
        ESP_LOGW(TAG, "crash dump: prune skipped (alloc failed)");
        free(found);
        free(seqs);
        return;
    }
    int n = 0;
    DIR *d = opendir(MICHI_LOG_LOGS_DIR);
    if (d == NULL) {
        free(seqs);
        free(found);
        return;
    }
    struct dirent *de;
    while (n < MAX_DUMPS && (de = readdir(d)) != NULL) {
        unsigned seq = 0;
        if (sscanf(de->d_name, "crash_%u.txt", &seq) == 1) {
            /* Accept only canonical names (crash_<seq>.txt, no leading
             * zeros): the path is rebuilt from the seq, so a foreign or
             * hand-made name must never be unlinked by pruning. */
            char canonical[48];
            snprintf(canonical, sizeof(canonical), "crash_%u.txt", seq);
            if (strcmp(canonical, de->d_name) == 0) {
                seqs[n] = seq;
                snprintf(found[n], sizeof(found[n]),
                         MICHI_LOG_CRASH_FILE_FMT, seq);
                n++;
            }
        }
    }
    closedir(d);
    if (n > keep) {
        /* insertion sort ascending by seq (oldest first) */
        for (int i = 1; i < n; i++) {
            const uint32_t s = seqs[i];
            char f[sizeof(found[0])];
            memcpy(f, found[i], sizeof(f));
            int j = i - 1;
            while (j >= 0 && seqs[j] > s) {
                seqs[j + 1] = seqs[j];
                memcpy(found[j + 1], found[j], sizeof(found[j]));
                j--;
            }
            seqs[j + 1] = s;
            memcpy(found[j + 1], f, sizeof(f));
        }
        for (int i = 0; i < n - keep; i++) {
            if (unlink(found[i]) != 0) {
                ESP_LOGW(TAG, "crash dump: prune %s failed", found[i]);
            } else {
                ESP_LOGI(TAG, "crash dump: pruned %s (keep=%d)",
                         found[i], keep);
            }
        }
    }
    free(seqs);
    free(found);
}

static void journal_flush_crash_dump(void)
{
    if (s_crash_staging == NULL || s_crash_staging_len == 0) {
        return;
    }
    char path[80];
    snprintf(path, sizeof(path), MICHI_LOG_CRASH_FILE_FMT,
             (unsigned)(s_boot_seq > 0 ? s_boot_seq - 1 : 0));
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGW(TAG, "crash dump: dump abandoned: open failed (%s)", path);
        heap_caps_free(s_crash_staging);
        s_crash_staging = NULL;
        s_crash_staging_len = 0;
        return;
    }
    /* F5/M1/M2b: verify the write AND the close - SPIFFS buffers, so an
     * error may surface only at fclose(); both are evaluated separately
     * so the close ALWAYS runs. When the write fails the usual cause is
     * a FULL SPIFFS: prune the old crash dumps first (frees space) and
     * retry ONCE before abandoning (converges: after the prune, either
     * the dump fits or SPIFFS is genuinely out of space). */
    const size_t expect = s_crash_staging_len;
    size_t written = fwrite(s_crash_staging, 1, expect, f);
    if (written != expect) {
        fclose(f);
        journal_prune_crash_dumps();
        f = fopen(path, "w");
        if (f == NULL) {
            ESP_LOGW(TAG, "crash dump: dump abandoned: reopen after "
                          "prune failed (%s)", path);
            journal_mark_degraded(expect);
            heap_caps_free(s_crash_staging);
            s_crash_staging = NULL;
            s_crash_staging_len = 0;
            return;
        }
        written = fwrite(s_crash_staging, 1, expect, f);
    }
    const int rc = fclose(f);
    if (written != expect || rc != 0) {
        ESP_LOGW(TAG, "crash dump: dump abandoned: write failed bytes=%u",
                 (unsigned)expect);
        journal_mark_degraded(expect);
        heap_caps_free(s_crash_staging);
        s_crash_staging = NULL;
        s_crash_staging_len = 0;
        return;
    }
    ESP_LOGI(TAG, "crash dump written: %s (%u bytes)",
             path, (unsigned)s_crash_staging_len);
    heap_caps_free(s_crash_staging);
    s_crash_staging = NULL;
    s_crash_staging_len = 0;
    journal_prune_crash_dumps();
}

static uint32_t file_size(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    uint32_t size = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long sz = ftell(f);
        if (sz > 0) {
            size = (uint32_t)sz;
        }
    }
    fclose(f);
    return size;
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

esp_err_t michi_log_init(void)
{
    if (s_prev_vprintf != NULL) {
        return ESP_OK; /* idempotent */
    }
    s_prev_vprintf = esp_log_set_vprintf(log_vprintf);

    const size_t tail_bytes = (size_t)CONFIG_MICHI_LOG_TAIL_SIZE_KB * 1024u;
    s_tail_ring = heap_caps_malloc(tail_bytes, MALLOC_CAP_SPIRAM);
    if (s_tail_ring == NULL) {
        ESP_LOGE(TAG, "tail ring alloc failed (%u KB PSRAM) - tail disabled",
                 CONFIG_MICHI_LOG_TAIL_SIZE_KB);
    } else {
        /* H1 (MS-11 on-device fix): the fresh ring header is initialized
         * BEFORE any log is emitted (crash_stage itself logs). The
         * PREVIOUS boot's header is snapshotted first so the crash dump
         * can still validate/read the prior ring contents instead of
         * trusting PSRAM garbage - which used to fault in log_vprintf
         * (StoreProhibited) and arm the crash path for the NEXT boot,
         * producing an infinite boot-crash loop on real hardware. */
        michi_log_ring_hdr_t prev;
        memcpy(&prev, s_tail_ring, sizeof(prev));
        const uint32_t slots = (uint32_t)((tail_bytes - sizeof(*s_tail_ring)) /
                                          MICHI_LOG_ENTRY_SLOT_BYTES);
        s_tail_ring->magic = MICHI_LOG_RING_MAGIC;
        s_tail_ring->slot_size = (uint32_t)sizeof(michi_log_entry_t);
        s_tail_ring->slots = slots;
        s_tail_ring->head = 0;
        s_tail_ring->count = 0;
        if (reset_is_crash()) {
            crash_stage(&prev);
        }
        ESP_LOGI(TAG, "tail ring: %u slots (%u KB PSRAM @%p)",
                 (unsigned)slots, CONFIG_MICHI_LOG_TAIL_SIZE_KB,
                 (void *)s_tail_ring);
    }

    s_journal_queue = xQueueCreate(CONFIG_MICHI_LOG_QUEUE_LEN,
                                   sizeof(michi_log_journal_item_t));
    if (s_journal_queue == NULL) {
        ESP_LOGE(TAG, "journal queue alloc failed - journal disabled");
    } else {
        const esp_err_t oerr = michi_state_register_observer(0,
                                                             log_state_observer);
        if (oerr != ESP_OK) {
            ESP_LOGE(TAG, "observer registration failed: %s - journal disabled",
                     esp_err_to_name(oerr));
        } else {
            s_observer_ok = true;
        }
    }
    /* F10: the header contract - only when BOTH layers failed does init
     * fail (console logging still works either way); a partial failure
     * degrades with the logs above and returns ESP_OK. */
    if (s_tail_ring == NULL && (s_journal_queue == NULL || !s_observer_ok)) {
        ESP_LOGE(TAG, "tail ring AND journal queue alloc failed - "
                      "only console logging works");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t michi_log_start_journal(void)
{
    if (s_prev_vprintf == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_journal_mounted) {
        return ESP_OK; /* idempotent */
    }
    if (s_journal_queue == NULL || !s_observer_ok) {
        return ESP_ERR_NO_MEM;
    }
    /* M3: a previous shutdown that timed out is UNCONFIRMED - the
     * s_journal_quit flag marks it. Re-arming would spawn a second
     * journal task while the presumed-gone one may still hold the queue
     * (and SPIFFS syscalls). Refuse instead of silently clearing the
     * flag: it is only cleared by a CONFIRMED exit (shutdown) or a
     * reboot (which also resolves the in-flight syscalls, per M4). */
    if (s_journal_quit) {
        ESP_LOGE(TAG, "journal: previous shutdown unconfirmed - start "
                      "refused until reboot");
        return ESP_ERR_INVALID_STATE;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = MICHI_LOG_SPIFFS_BASE,
        .partition_label = MICHI_LOG_SPIFFS_PARTITION,
        .max_files = 8,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount on '%s' failed: %s - journal disabled "
                      "(tail keeps working); first boot needs a formatted "
                      "partition",
                 MICHI_LOG_SPIFFS_PARTITION, esp_err_to_name(err));
        s_journal_mounted = false;
        /* F8: a staged crash dump cannot be flushed - abandon it with a
         * clear reason instead of keeping a dangling staging. */
        if (s_crash_staging != NULL) {
            ESP_LOGW(TAG, "crash dump: dump abandoned: spiffs mount failed");
            heap_caps_free(s_crash_staging);
            s_crash_staging = NULL;
            s_crash_staging_len = 0;
        }
        return err;
    }
    s_journal_mounted = true;

    nvs_handle_t h;
    err = nvs_open(MICHI_LOG_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open('%s') failed: %s - boot_seq starts at 1",
                 MICHI_LOG_NVS_NS, esp_err_to_name(err));
        s_boot_seq = 1;
    } else {
        uint32_t seq = 0;
        if (nvs_get_u32(h, MICHI_LOG_NVS_BOOT_SEQ_KEY, &seq) != ESP_OK) {
            seq = 0;
        }
        s_boot_seq = (seq == UINT32_MAX) ? UINT32_MAX : seq + 1;
        /* F17/M6: the boot_seq is the crash-dump naming key - a failed
         * SET or COMMIT means the next boot may reuse a seq and
         * overwrite a dump. Both are checked (no silent short-circuit:
         * a set error is reported even when the commit succeeds). */
        const esp_err_t set_err = nvs_set_u32(h, MICHI_LOG_NVS_BOOT_SEQ_KEY,
                                              s_boot_seq);
        const esp_err_t commit_err = nvs_commit(h);
        if (set_err != ESP_OK || commit_err != ESP_OK) {
            ESP_LOGW(TAG, "journal: boot_seq persist failed (set=%s "
                          "commit=%s) - next boot may overwrite a dump",
                     esp_err_to_name(set_err), esp_err_to_name(commit_err));
        }
        nvs_close(h);
    }

    /* M5: reset the degraded latch BEFORE the flush - a flush that fails
     * (e.g. SPIFFS full) re-latches so the 'events not written until
     * restart' warning (journal_mark_degraded) is only emitted when it
     * is actually true. */
    s_journal_degraded = false;

    journal_flush_crash_dump();

    /* F5: a fresh start re-arms the journal. NOTE: the quit flag is NOT
     * cleared here (M3) - it is the unconfirmed-shutdown marker and is
     * only cleared by a confirmed exit in michi_log_shutdown() or a
     * reboot. */

    BaseType_t t = xTaskCreate(journal_task, "michi_log",
                               CONFIG_MICHI_LOG_TASK_STACK_BYTES, NULL,
                               MICHI_LOG_TASK_PRIORITY, &s_journal_task);
    if (t != pdPASS) {
        ESP_LOGE(TAG, "journal task create failed - journal disabled");
        s_journal_mounted = false;
        /* F11: unregister SPIFFS so journal_available() reports false and
         * a retry of start_journal() can remount cleanly. */
        esp_vfs_spiffs_unregister(MICHI_LOG_SPIFFS_PARTITION);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "journal: mounted=%s boot_seq=%u tail_psram=%s",
             s_journal_mounted ? "yes" : "no", (unsigned)s_boot_seq,
             s_tail_ring != NULL ? "yes" : "no");
    if (s_dropped_events > 0) {
        ESP_LOGW(TAG, "journal: %u events dropped (queue full before task start)",
                 (unsigned)s_dropped_events);
    }
    return ESP_OK;
}

esp_err_t michi_log_get_tail(char *out, size_t out_len, uint32_t max_entries)
{
    if (out == NULL || out_len == 0 || max_entries == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tail_ring == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    out[0] = '\0';
    size_t used = 0;
    michi_log_entry_t *entry = (michi_log_entry_t *)((uint8_t *)s_tail_ring +
                                                     sizeof(*s_tail_ring));
    portENTER_CRITICAL(&s_tail_mux);
    const uint32_t slots = s_tail_ring->slots;
    const uint32_t head = s_tail_ring->head;
    uint32_t count = s_tail_ring->count;
    if (count > max_entries) {
        count = max_entries;
    }
    const uint32_t start = (head + slots - count) % slots;
    for (uint32_t i = 0; i < count; i++) {
        const michi_log_entry_t *e = &entry[(start + i) % slots];
        const size_t plen = (e->len > 0 && e->len <= MICHI_LOG_ENTRY_PAYLOAD_MAX)
                                ? (size_t)e->len - 1
                                : 0;
        const int w = snprintf(out + used, out_len - used,
                               MICHI_LOG_TAIL_LINE_FMT,
                               (int)e->level, e->t_ms,
                               plen > 0 ? e->payload : "");
        if (w < 0 || (size_t)w >= out_len - used) {
            break;
        }
        used += (size_t)w;
    }
    portEXIT_CRITICAL(&s_tail_mux);
    return ESP_OK;
}

esp_err_t michi_log_get_journal(char *out, size_t out_len, uint32_t offset,
                                uint32_t *next_offset)
{
    if (out == NULL || out_len == 0 || next_offset == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_journal_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    const int files = CONFIG_MICHI_LOG_JOURNAL_FILES;
    char paths[4][64];
    uint32_t sizes[4] = {0};
    uint64_t total = 0;
    for (int i = 0; i < files; i++) {
        journal_path(paths[i], sizeof(paths[i]), i + 1);
        sizes[i] = file_size(paths[i]);
        total += sizes[i];
    }
    if ((uint64_t)offset >= total) {
        *next_offset = offset;
        out[0] = '\0';
        return ESP_OK;
    }
    uint32_t rem = offset;
    int target = -1;
    uint32_t file_off = 0;
    for (int i = 0; i < files; i++) {
        if (rem < sizes[i]) {
            target = i;
            file_off = rem;
            break;
        }
        rem -= sizes[i];
    }
    if (target < 0) { /* defensive: sizes raced with rotation */
        *next_offset = offset;
        out[0] = '\0';
        return ESP_OK;
    }
    FILE *f = fopen(paths[target], "r");
    if (f == NULL) {
        *next_offset = offset;
        out[0] = '\0';
        return ESP_OK;
    }
    size_t n = 0;
    if (fseek(f, (long)file_off, SEEK_SET) == 0) {
        n = fread(out, 1, out_len - 1, f);
    }
    fclose(f);
    if (n == 0) {
        out[0] = '\0';
        *next_offset = offset;
        return ESP_OK;
    }
    /* Trim to the last complete line; the partial remainder is re-read
     * from *next_offset on the next page (seekable file). */
    size_t cut = 0;
    for (size_t i = 0; i < n; i++) {
        if (out[i] == '\n') {
            cut = i + 1;
        }
    }
    out[cut] = '\0';
    if (cut == 0) {
        /* A single line longer than the page: skipped, documented. */
        *next_offset = offset + (uint32_t)n;
        return ESP_OK;
    }
    *next_offset = offset + (uint32_t)cut;
    return ESP_OK;
}

uint32_t michi_log_get_boot_seq(void)
{
    return s_boot_seq;
}

bool michi_log_tail_available(void)
{
    return s_tail_ring != NULL;
}

bool michi_log_journal_available(void)
{
    return s_journal_mounted;
}

esp_err_t michi_log_shutdown(void)
{
    if (s_journal_task != NULL) {
        /* F3: save OUR handle under the mux BEFORE enqueuing QUIT - the
         * task notifies this handle (never its own). */
        bool flag_path = false;
        portENTER_CRITICAL(&s_journal_mux);
        s_shutdown_done = xTaskGetCurrentTaskHandle();
        portEXIT_CRITICAL(&s_journal_mux);
        michi_log_journal_item_t quit = {.kind = MICHI_LOG_ITEM_QUIT};
        if (xQueueSend(s_journal_queue, &quit, 0) == pdTRUE) {
            /* The quit item is enqueued BEHIND pending events (FIFO):
             * they flush first, then the task exits and notifies. */
        } else {
            /* Queue full: the QUIT never lands - set the flag the task
             * polls and notify it directly (both under the mux). */
            flag_path = true;
            portENTER_CRITICAL(&s_journal_mux);
            s_journal_quit = true;
            const TaskHandle_t task = s_journal_task;
            portEXIT_CRITICAL(&s_journal_mux);
            xTaskNotifyGive(task);
        }
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)) == 0) {
            /* M3/M4: UNCONFIRMED shutdown - the task is presumed stuck
             * in a SPIFFS syscall. Mark it with the quit flag (a later
             * start_journal() is refused until reboot) and DO NOT
             * unregister SPIFFS: the in-flight syscall still owns it
             * (convergence depends on those syscalls completing, which
             * the reboot guarantees). */
            portENTER_CRITICAL(&s_journal_mux);
            s_journal_quit = true;
            portEXIT_CRITICAL(&s_journal_mux);
            ESP_LOGW(TAG, "journal: shutdown timed out, SPIFFS left "
                          "mounted (start refused until reboot)");
            s_journal_task = NULL;
            return ESP_OK;
        }
        if (flag_path) {
            /* Confirmed exit: the quit flag served its purpose and the
             * journal may re-arm on a later start_journal(). */
            portENTER_CRITICAL(&s_journal_mux);
            s_journal_quit = false;
            portEXIT_CRITICAL(&s_journal_mux);
        }
        s_journal_task = NULL;
    }
    if (s_journal_mounted) {
        esp_vfs_spiffs_unregister(MICHI_LOG_SPIFFS_PARTITION);
        s_journal_mounted = false;
    }
    return ESP_OK;
}
