#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Wall-clock owner for signed discovery (P0-02): SNTP via the ESP-IDF
 * 5.3 esp_netif_sntp.h API (esp_netif_sntp_init/start/deinit +
 * esp_netif_sntp_sync_wait), NOT the deprecated lwip API.
 *
 * The signed UDP announce carries a timestamp_ms that Michi Link
 * rejects outside +-90 s of its own clock. Without SNTP the announce
 * would carry 0 and be rejected anyway, so michi_discovery gates the
 * signed announce on michi_time_is_synchronized() and takes the
 * timestamp from michi_time_unix_ms().
 *
 * Lifecycle (owned by michi_wifi, the network bring-up owner):
 * - init()   boot: configure SNTP (start = false) + spawn the sync task.
 * - start()  GOT_IP: (re)start the SNTP client and let the sync task run
 *            a BOUNDED sync_wait (timeout + retries from Kconfig).
 * - stop()   disconnect: CONSERVE the sync state (documented policy).
 * - shutdown() before wifi teardown: join the sync task, deinit SNTP.
 *
 * Synchronization policy (documented, required by the hardening plan):
 * - synchronized becomes true ONLY on the first successful SNTP sync.
 * - On a reconnect the task re-validates: a FRESH sync event (recorded
 *   in the SNTP time-sync callback with the monotonic esp_timer clock)
 *   must land inside the bounded window. A stale sync semaphore from a
 *   previous sync is NOT accepted as a fresh sync.
 * - If the re-validation window elapses without a fresh sync the last
 *   state is CONSERVED: the wall clock keeps advancing from the RTC
 *   (set once by SNTP) and its drift stays far inside the +-90 s
 *   announce window even over long outages. Not-synchronized is only
 *   ever the pre-first-sync state (or after shutdown).
 * - SNTP errors never block the firmware: a failed init/start leaves
 *   discovery gated and everything else running.
 *
 * IDF 5.3 note: esp_netif_sntp.h exposes NO stop() function.
 * esp_netif_sntp_start() internally stops + restarts the lwIP SNTP
 * client, and between start() calls the client simply has no reachable
 * server (no IP), so an explicit stop on link-down is not required;
 * stop() therefore only records the link-down for the state policy.
 */

/** Called when a FRESH SNTP sync lands (from the michi_time sync task
 *  context, never from an ISR). Discovery uses it to resume announcing
 *  immediately instead of waiting for the next 30 s tick. */
typedef void (*michi_time_sync_cb_t)(void *ctx);

/**
 * @brief Initialize the time subsystem: configure the SNTP client
 *        (start = false, one server from Kconfig) and spawn the sync
 *        task (blocked until start()).
 *
 * No network traffic until start(). Idempotent while initialized.
 *
 * @return ESP_OK; SNTP init errors and task-creation errors are
 *         propagated (the caller keeps running degraded: discovery
 *         stays gated, everything else unaffected).
 */
esp_err_t michi_time_init(void);

/**
 * @brief Network up (GOT_IP): (re)start the SNTP client and kick the
 *        sync task, which runs a bounded sync_wait (Kconfig timeout +
 *        retries) and flips the synchronized state on a FRESH sync.
 *
 * Safe to call on every GOT_IP (IP renewals included): start()
 * restarts the client and the task re-validates.
 *
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init or when SNTP init
 *         failed; esp_netif_sntp_start errors propagated. Callers log
 *         and continue - announces stay gated until a sync lands.
 */
esp_err_t michi_time_start(void);

/**
 * @brief Network down (disconnect): record the link-down. The
 *        synchronized state is CONSERVED (see the header policy):
 *        stop() never flips it. Idempotent.
 *
 * @return ESP_OK.
 */
esp_err_t michi_time_stop(void);

/**
 * @brief Shut the time subsystem down: join the sync task (bounded by
 *        one sync_wait timeout + slack) and deinit SNTP. After this,
 *        is_synchronized() returns false and unix_ms() returns 0.
 *        Idempotent.
 *
 * @return ESP_OK.
 */
esp_err_t michi_time_shutdown(void);

/**
 * @brief Whether the wall clock is synchronized (at least one
 *        successful SNTP sync this boot; conserved across outages).
 */
bool michi_time_is_synchronized(void);

/**
 * @brief Current wall time in Unix epoch ms.
 *
 * @return time(NULL) * 1000 when synchronized; **0 when NOT
 *         synchronized**. This function never hands out a silently
 *         invalid timestamp: callers MUST gate on
 *         michi_time_is_synchronized() before using the value (the
 *         only caller, michi_discovery, does exactly that).
 */
int64_t michi_time_unix_ms(void);

/**
 * @brief Name of the wall-clock source ("sntp", the only source
 *        supported). Diagnostics-only; not sensitive.
 */
const char *michi_time_sync_source(void);

/**
 * @brief Register the callback invoked on every FRESH sync (single
 *        slot - discovery is the only consumer). Runs from the sync
 *        task context: keep it bounded and non-blocking.
 *
 * @param cb  Callback (NULL clears the slot).
 * @param ctx Opaque argument passed to cb.
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init.
 */
esp_err_t michi_time_register_sync_cb(michi_time_sync_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
