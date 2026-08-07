#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shared geometry of the tail ring, exported so michi_http can size its
 * per-entry buffers without magic numbers. */
#define MICHI_LOG_ENTRY_PAYLOAD_MAX 504 /* bytes of payload per ring slot */
#define MICHI_LOG_TAIL_LINE_FMT "%c %010" PRIu32 " %s\n" /* get_tail() line */
#define MICHI_LOG_TAIL_PREFIX_LEN 13 /* "<level> <t_ms:010u> " before payload */
/* Worst-case rendered line: prefix + payload + '\n' + NUL. */
#define MICHI_LOG_TAIL_LINE_MAX \
    (MICHI_LOG_TAIL_PREFIX_LEN + MICHI_LOG_ENTRY_PAYLOAD_MAX + 2)

/**
 * @brief Hybrid log registry (phase 16): volatile tail + durable journal.
 *
 * Three layers:
 *
 * 1. TAIL (volatile, PSRAM): an esp_log_set_vprintf override writes every
 *    ESP_LOG* line into a fixed ring buffer allocated from PSRAM at boot
 *    (the FIRST PSRAM allocation, so the address is deterministic across
 *    boots of the same build). A crash that restarts the chip leaves the
 *    ring content in PSRAM; on the next boot with a crash reset reason
 *    (PANIC/INT_WDT/TASK_WDT/WDT) the last MICHI_LOG_CRASH_DUMP_KB are
 *    staged and flushed to /spiffs/logs/crash_<boot_seq_prev>.txt once
 *    SPIFFS is mounted. If the magic header does not match (fresh PSRAM,
 *    different build, PSRAM not first allocation) the dump is skipped -
 *    never invented.
 *
 * 2. JOURNAL (durable, SPIFFS partition "storage"): only STATE CHANGES
 *    and ERRORS, not every log line. A michi_state observer (filter 0,
 *    one slot) forwards MICHI_EVENT_STATE_CHANGED / MICHI_EVENT_ERROR /
 *    MICHI_EVENT_UPDATE_FAILED to a FreeRTOS queue; the journal task
 *    appends one text line per event to /spiffs/logs/journal.1 and
 *    rotates (rename chain, drop the oldest) when the active file exceeds
 *    MICHI_LOG_JOURNAL_MAX_KB. michi_state_report_error() posts the same
 *    events to the bus, so errors reported that way land here too.
 *    Source: the OBSERVED EVENTS - the FSM's last-error slot is not
 *    consulted.
 *
 * 3. ENDPOINT: GET /api/v1/receiver/logs (Bearer STATUS) serves the tail
 *    as JSON lines and the journal as offset-paginated pages
 *    (michi_http). The tail payload is the raw ESP_LOG payload (tag +
 *    key=value), already checked by the project's zero-secret rule: no
 *    token/challenge/nonce value is ever part of a log format string.
 *
 * Degradation contract: the tail and the journal are independent. A
 * failed SPIFFS mount disables only the journal (clear log, boot
 * continues); a failed PSRAM allocation disables only the tail; the
 * console output is ALWAYS preserved (the previous vprintf is chained).
 */

/**
 * @brief Install the log subsystem. VERY EARLY in app_main (before NVS).
 *
 * - captures the previous vprintf and replaces it (console output is
 *   chained, never lost);
 * - allocates the tail ring from PSRAM (first PSRAM allocation);
 * - checks the crash-dump condition (reset reason + old ring magic) and
 *   stages the dump for start_journal();
 * - creates the journal queue and registers the FSM observer.
 *
 * Safe to call once; repeated calls return ESP_OK (idempotent). The
 * journal task is NOT started here: start_journal() runs after NVS.
 *
 * @return ESP_OK; ESP_ERR_NO_MEM when neither the ring nor the queue
 *         could be created (console logging still works).
 */
esp_err_t michi_log_init(void);

/**
 * @brief Start the durable journal. AFTER init_nvs().
 *
 * - mounts SPIFFS on the "storage" partition (format_if_mount_failed =
 *   false: a failed mount is logged and degrades to tail-only);
 * - reads/increments boot_seq (NVS namespace "michi_log", key
 *   "boot_seq"; saturates at UINT32_MAX - documented, no wrap);
 * - flushes a staged crash dump to /spiffs/logs/crash_<prev>.txt;
 * - starts the journal task (priority 3).
 *
 * @return ESP_OK; ESP_ERR_INVALID_STATE when init() was not called;
 *         the SPIFFS mount error when it failed (journal disabled,
 *         tail unaffected).
 */
esp_err_t michi_log_start_journal(void);

/**
 * @brief Copy the newest tail entries as text lines into out.
 *
 * One line per entry, NUL-terminated text, newest last (chronological):
 *   "<level> <t_ms:010u> <payload>\n"
 * where <level> is I/W/E/D/V (or '?' for non-esp_log formats) and
 * <payload> is the raw ESP_LOG line WITHOUT the leading level/timestamp
 * prefix (tag + key=value included). Entries longer than the ring slot
 * (504 bytes payload) are truncated in the tail; the console is not.
 *
 * @param out         Output buffer (must not be NULL).
 * @param out_len     Buffer size; the real limit of what is returned.
 * @param max_entries Maximum entries to emit.
 * @return ESP_OK; ESP_ERR_INVALID_ARG on NULL/zero args;
 *         ESP_ERR_INVALID_STATE when the tail ring is unavailable
 *         (no PSRAM at init).
 */
esp_err_t michi_log_get_tail(char *out, size_t out_len, uint32_t max_entries);

/**
 * @brief Copy a page of the journal (offset-based pagination).
 *
 * Reads from journal.1..journal.N (rotation order, journal.1 newest)
 * starting at byte offset. The output is trimmed to the last complete
 * line; *next_offset points past it (repeat the request with that value
 * to get the next page). A line longer than the page buffer is skipped
 * (journal lines are short by construction - documented).
 *
 * Sentinel: *next_offset == offset means the END of the journal - stop
 * paging (the requested offset is past the last byte).
 *
 * @param out         Output buffer (must not be NULL).
 * @param out_len     Buffer size (page).
 * @param offset      Byte offset into the journal (>= 0).
 * @param next_offset On success: offset for the next page.
 * @return ESP_OK; ESP_ERR_INVALID_ARG on NULL/zero args;
 *         ESP_ERR_INVALID_STATE when the journal is unavailable.
 */
esp_err_t michi_log_get_journal(char *out, size_t out_len, uint32_t offset,
                                uint32_t *next_offset);

/**
 * @brief Current boot sequence number (0 until start_journal()).
 *
 * @return The boot sequence (>= 1 after start_journal(); 0 before).
 */
uint32_t michi_log_get_boot_seq(void);

/**
 * @brief true when the tail ring is available (PSRAM allocated at init).
 *
 * @return true when michi_log_get_tail() can be called.
 */
bool michi_log_tail_available(void);

/**
 * @brief true when the journal is available (SPIFFS mounted).
 *
 * @return true when michi_log_get_journal() can be called.
 */
bool michi_log_journal_available(void);

/**
 * @brief Stop the journal task (flushes in-flight events) and unregister
 *        SPIFFS. Idempotent; safe when the journal was never started.
 *
 * Shutdown handshake contract: the CALLER's task handle is saved under
 * the journal mux BEFORE the QUIT item is enqueued; the journal task
 * notifies THAT handle (never itself) after it has drained the queue and
 * exited. If the QUIT cannot be enqueued (queue full), the s_journal_quit
 * flag is set under the mux and the journal task is notified directly -
 * it polls the flag with a bounded receive timeout. The caller waits on
 * its own handle for at most 500 ms; on timeout a clear warning is
 * logged (the task is presumed gone - the flag guarantees it exits).
 *
 * @return ESP_OK.
 */
esp_err_t michi_log_shutdown(void);

#ifdef __cplusplus
}
#endif
