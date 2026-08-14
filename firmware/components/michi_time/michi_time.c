/*
 * Wall-clock owner for signed discovery (P0-02).
 *
 * SNTP synchronization through the ESP-IDF 5.3 esp_netif_sntp.h API
 * (esp_netif_sntp_init/start/deinit + esp_netif_sntp_sync_wait) - the
 * deprecated lwip sntp_* API is NOT used. See include/michi_time.h for
 * the full contract and the documented synchronization policy.
 *
 * Concurrency model:
 * - The SNTP time-sync callback (sntp_sync_cb) runs in the lwIP tcpip
 *   thread context and only records the monotonic timestamp of the last
 *   sync event (a volatile int64_t store) - nothing else.
 * - All waiting happens in the dedicated sync task ("michi_time_sync",
 *   low priority, blocked on a binary semaphore): after a kick from
 *   start() it runs BOUNDED esp_netif_sntp_sync_wait rounds (Kconfig
 *   timeout + retries) and, on a FRESH sync, flips the synchronized
 *   flag and invokes the registered user callback (discovery resumes
 *   announcing immediately). The esp_event task (michi_wifi GOT_IP)
 *   is never blocked.
 * - Shutdown joins the task cooperatively through a binary "done"
 *   semaphore (bounded by one sync_wait timeout + slack), mirroring the
 *   provisioning-task join pattern, then deinitializes SNTP.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"

#include "michi_time.h"

#define TAG "michi_time"

/* The sync task sits below the provisioning task (3) and above the LED
 * animation (3 vs 2): it blocks on semaphores ~forever and only runs a
 * few instructions per sync event. */
#define MICHI_TIME_TASK_PRIORITY 2

/* Shutdown join slack on top of the longest possible sync_wait block
 * (one attempt timeout): the task can be mid-wait when shutdown kicks. */
#define MICHI_TIME_JOIN_SLACK_MS 2000

static volatile bool s_initialized;
static volatile bool s_synchronized;
static volatile bool s_shutdown;

static TaskHandle_t s_sync_task;
/* Kick: binary semaphore start() gives, the sync task takes. */
static SemaphoreHandle_t s_kick;
/* Done: binary semaphore the sync task gives at exit (shutdown join). */
static SemaphoreHandle_t s_done;

/* Monotonic timestamp (esp_timer) of the last SNTP sync event, recorded
 * by the SNTP time-sync callback (tcpip thread); read by the sync task.
 * A FRESH sync for the current validation round means
 * s_last_sync_us >= s_sync_epoch_us (epoch captured at start()). */
static volatile int64_t s_last_sync_us;
static volatile int64_t s_sync_epoch_us;

static michi_time_sync_cb_t s_sync_cb;
static void *s_sync_ctx;

/* ------------------------------------------------------------------ */
/* Internals                                                          */
/* ------------------------------------------------------------------ */

/* SNTP time-sync notification (lwIP tcpip thread context): record the
 * monotonic sync timestamp only - never call out of this context. */
static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_last_sync_us = esp_timer_get_time();
}

static bool sync_is_fresh(void)
{
    /* A fresh sync event landed after this validation round started. A
     * stale binary-semaphore give from an earlier sync (idle re-sync,
     * previous round) is NOT accepted as a fresh sync - the round keeps
     * waiting/retrying for a real event. */
    return s_last_sync_us >= s_sync_epoch_us;
}

static void run_sync_callbacks(void)
{
    const michi_time_sync_cb_t cb = s_sync_cb;
    if (cb != NULL) {
        cb(s_sync_ctx);
    }
}

/* Dedicated sync task: kicked by start(), runs bounded sync_wait
 * rounds, flips the synchronized state on a FRESH sync and invokes the
 * user callback (immediate announce resume). */
static void sync_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_kick, portMAX_DELAY);
        if (s_shutdown || !s_initialized) {
            break;
        }

        esp_err_t sync_err = ESP_ERR_TIMEOUT;
        for (int attempt = 0;
             attempt < CONFIG_MICHI_TIME_SYNC_RETRIES &&
             !s_shutdown && s_initialized;
             attempt++) {
            sync_err = esp_netif_sntp_sync_wait(
                pdMS_TO_TICKS(CONFIG_MICHI_TIME_SYNC_TIMEOUT_MS));
            if (sync_err == ESP_OK && sync_is_fresh()) {
                break;
            }
            if (s_shutdown || !s_initialized) {
                break;
            }
            if (sync_err != ESP_OK) {
                ESP_LOGW(TAG, "time: sync attempt %d/%d failed (%s) - "
                         "retrying", attempt + 1,
                         CONFIG_MICHI_TIME_SYNC_RETRIES,
                         esp_err_to_name(sync_err));
            }
            /* IDF 5.3: start() stops and restarts the lwIP SNTP client
             * (fresh query burst). Also re-arms the round after a stale
             * (not fresh) semaphore give. */
            const esp_err_t restart_err = esp_netif_sntp_start();
            if (restart_err != ESP_OK) {
                ESP_LOGW(TAG, "time: SNTP restart failed: %s",
                         esp_err_to_name(restart_err));
            }
        }
        if (s_shutdown || !s_initialized) {
            break;
        }

        if (sync_err == ESP_OK && sync_is_fresh()) {
            const bool first = !s_synchronized;
            s_synchronized = true;
            ESP_LOGI(TAG, "time: %s via %s (unix_ms=%" PRId64 ")",
                     first ? "synchronized" : "resynchronized",
                     michi_time_sync_source(), michi_time_unix_ms());
            run_sync_callbacks();
        } else if (s_synchronized) {
            /* Documented policy (header): an outage never drops the
             * last sync state - the RTC keeps the wall clock advancing
             * and the drift stays far inside the +-90 s announce
             * window. */
            ESP_LOGW(TAG, "time: revalidation failed - conserving last "
                     "synchronized state (RTC keeps the clock)");
        } else {
            ESP_LOGW(TAG, "time: sync failed after %d attempt(s) - "
                     "clock stays unsynchronized, announces stay gated",
                     CONFIG_MICHI_TIME_SYNC_RETRIES);
        }
    }

    /* Cooperative join (same pattern as the provisioning task): signal
     * the waiting shutdown caller, then self-delete. */
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t michi_time_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* IDF 5.3 esp_sntp_config_t. start = false: the client is started
     * explicitly on GOT_IP (michi_time_start). The time-sync callback
     * records the monotonic sync timestamp for freshness checks; the
     * bounded wait itself runs in the sync task. */
    const esp_sntp_config_t config = {
        .smooth_sync = false,
        .server_from_dhcp = false,
        .wait_for_sync = true,
        .start = false,
        .sync_cb = sntp_sync_cb,
        .renew_servers_after_new_IP = false,
        .index_of_first_server = 0,
        .num_of_servers = 1,
        .servers = { CONFIG_MICHI_TIME_SNTP_SERVER },
    };
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "time: SNTP init failed: %s - signed announces "
                 "stay gated", esp_err_to_name(err));
        return err;
    }

    s_kick = xSemaphoreCreateBinary();
    s_done = xSemaphoreCreateBinary();
    if (s_kick == NULL || s_done == NULL) {
        if (s_kick != NULL) {
            vSemaphoreDelete(s_kick);
        }
        if (s_done != NULL) {
            vSemaphoreDelete(s_done);
        }
        s_kick = NULL;
        s_done = NULL;
        esp_netif_sntp_deinit();
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t rc = xTaskCreate(sync_task, "michi_time_sync",
                                      CONFIG_MICHI_TIME_TASK_STACK_BYTES,
                                      NULL, MICHI_TIME_TASK_PRIORITY,
                                      &s_sync_task);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "time: sync task creation failed");
        vSemaphoreDelete(s_kick);
        vSemaphoreDelete(s_done);
        s_kick = NULL;
        s_done = NULL;
        esp_netif_sntp_deinit();
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "subsystem=time state=ok server=%s",
             CONFIG_MICHI_TIME_SNTP_SERVER);
    return ESP_OK;
}

esp_err_t michi_time_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Monotonic epoch of THIS validation round: only sync events after
     * this point count as fresh (a stale semaphore give from a previous
     * sync is consumed by the task and retried). */
    s_sync_epoch_us = esp_timer_get_time();

    /* IDF 5.3: (re)starts the lwIP SNTP client - on a reconnect this
     * issues a fresh query burst. */
    const esp_err_t err = esp_netif_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "time: SNTP start failed: %s - announces stay "
                 "gated", esp_err_to_name(err));
        return err;
    }
    /* Wake the sync task; it takes the bounded wait. The give is lost
     * harmlessly if the task is still processing a previous round. */
    xSemaphoreGive(s_kick);
    return ESP_OK;
}

esp_err_t michi_time_stop(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }
    /* Documented policy (header): CONSERVE the sync state - stop()
     * never flips it. IDF 5.3 has no esp_netif_sntp_stop; the client
     * simply loses reachability without an IP and the next start()
     * restarts it. */
    ESP_LOGI(TAG, "time: link down - sync state conserved "
             "(synchronized=%d)", (int)s_synchronized);
    return ESP_OK;
}

esp_err_t michi_time_shutdown(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }
    s_initialized = false;
    s_shutdown = true;

    /* Cooperative join: wake the task (it may be mid sync_wait - the
     * wait is bounded by one attempt timeout) and wait for its "done"
     * signal. On timeout the task still exits shortly after (it checks
     * the shutdown flag after every wait) and the deinit below safely
     * unblocks any pending sync_wait (FreeRTOS vQueueDelete unblocks
     * waiters). */
    xSemaphoreGive(s_kick);
    const TickType_t join_ticks =
        pdMS_TO_TICKS(CONFIG_MICHI_TIME_SYNC_TIMEOUT_MS +
                      MICHI_TIME_JOIN_SLACK_MS);
    if (!xSemaphoreTake(s_done, join_ticks)) {
        ESP_LOGW(TAG, "time: sync task did not stop in %d ms",
                 CONFIG_MICHI_TIME_SYNC_TIMEOUT_MS +
                     MICHI_TIME_JOIN_SLACK_MS);
    }

    esp_netif_sntp_deinit();
    if (s_kick != NULL) {
        vSemaphoreDelete(s_kick);
        s_kick = NULL;
    }
    if (s_done != NULL) {
        vSemaphoreDelete(s_done);
        s_done = NULL;
    }
    s_sync_cb = NULL;
    s_synchronized = false;
    s_shutdown = false;
    ESP_LOGI(TAG, "subsystem=time state=off");
    return ESP_OK;
}

bool michi_time_is_synchronized(void)
{
    return s_initialized && s_synchronized;
}

int64_t michi_time_unix_ms(void)
{
    if (!michi_time_is_synchronized()) {
        /* Never hand out a silently invalid timestamp: 0 means "no
         * wall clock yet" and callers gate on is_synchronized(). */
        return 0;
    }
    return (int64_t)time(NULL) * 1000;
}

const char *michi_time_sync_source(void)
{
    return "sntp";
}

esp_err_t michi_time_register_sync_cb(michi_time_sync_cb_t cb, void *ctx)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_sync_cb = cb;
    s_sync_ctx = ctx;
    return ESP_OK;
}
