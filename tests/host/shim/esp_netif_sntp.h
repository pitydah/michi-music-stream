#pragma once
/* Shim for host-side tests: esp_netif_sntp.h stand-in mirroring the
 * ESP-IDF 5.3 API (esp_netif_sntp_init/start/deinit/sync_wait + the
 * esp_sntp_config_t struct, byte-compatible field names/order so the
 * REAL michi_time.c compiles unchanged). TEST-ONLY: never compiled
 * into firmware.
 *
 * Behavior mirrors esp_netif_sntp.c 5.3:
 *  - init(): stores the config; wait_for_sync=true creates the internal
 *    sync semaphore; start=true would auto-start (michi_time passes
 *    start=false and starts explicitly on GOT_IP).
 *  - A sync event (test_sntp_fire_sync) gives the sync semaphore and
 *    THEN invokes the configured sync_cb (same order as 5.3).
 *  - start(): restarts the client (stop + init semantics).
 *  - sync_wait(tout): bounded wait on the sync semaphore; ESP_OK /
 *    ESP_ERR_TIMEOUT / ESP_ERR_INVALID_STATE.
 *
 * Test hooks: fire sync events, fail injections for init/start, and the
 * fake wall clock (backed by time_shim.c - the time() override).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t TickType_t;

#define ESP_SNTP_SERVER_LIST(...) { __VA_ARGS__ }

typedef void (*esp_sntp_time_cb_t)(struct timeval *tv);

#define ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(servers_in_list, list_of_servers) \
    {                                                                           \
        .smooth_sync = false,                                                   \
        .server_from_dhcp = false,                                              \
        .wait_for_sync = true,                                                  \
        .start = true,                                                          \
        .sync_cb = NULL,                                                        \
        .renew_servers_after_new_IP = false,                                    \
        .index_of_first_server = 0,                                             \
        .num_of_servers = (servers_in_list),                                    \
        .servers = list_of_servers,                                             \
    }

#define ESP_NETIF_SNTP_DEFAULT_CONFIG(server) \
    ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(1, { server })

typedef struct esp_sntp_config {
    bool smooth_sync;
    bool server_from_dhcp;
    bool wait_for_sync;
    bool start;
    esp_sntp_time_cb_t sync_cb;
    bool renew_servers_after_new_IP;
    size_t index_of_first_server;
    size_t num_of_servers;
    const char *servers[CONFIG_LWIP_SNTP_MAX_SERVERS];
} esp_sntp_config_t;

esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config);
esp_err_t esp_netif_sntp_start(void);
void esp_netif_sntp_deinit(void);
esp_err_t esp_netif_sntp_sync_wait(TickType_t tout);

/* --- test hooks (TEST-ONLY) --- */

/* Reset all shim state (config, semaphore, counters, fail flags). */
void test_sntp_reset(void);

/* Simulate a server reply: sets the fake wall clock, gives the internal
 * sync semaphore and invokes the configured sync_cb (5.3 order). */
void test_sntp_fire_sync(uint64_t unix_sec);

/* Force esp_netif_sntp_init / esp_netif_sntp_start to fail. */
void test_sntp_set_fail_init(bool fail);
void test_sntp_set_fail_start(bool fail);

/* Counters / state inspection. */
int test_sntp_init_count(void);
int test_sntp_start_count(void);
bool test_sntp_client_started(void);

#ifdef __cplusplus
}
#endif
