/* Signed discovery runtime (MS-05): mDNS service + UDP multicast
 * announce with the canonical Michi Link signed identity.
 *
 * Owns the announce machinery; michi_wifi calls start()/stop() on
 * network up/down. See include/michi_discovery.h for the contract.
 *
 * Concurrency model (same rules as the former wifi mDNS code, F10):
 * the mdns v1.x API takes an internal semaphore with portMAX_DELAY, so
 * it can block and must never run under a portMUX critical section; the
 * announce mutex serializes start/stop/advertise/announce across the
 * esp_event task (michi_wifi GOT_IP/disconnect) and the esp_timer task
 * (periodic tick). Bounded waits everywhere: a contended tick is
 * skipped, never blocked. The socket is only touched under the mutex
 * (opened on network up, closed on network down/IP change - contract
 * 2.2).
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "mdns.h"

#include "discovery_nvs.h"
#include "michi_discovery.h"
#include "michi_identity.h"
#include "michi_product_profile.h"
#include "michi_time.h"

#define TAG "michi_discovery"

/* Bounded wait for the announce mutex (see the file header). */
#define MICHI_DISCOVERY_LOCK_MS 500

/* IPv4 dotted-quad max ("255.255.255.255" + NUL). */
#define MICHI_DISCOVERY_IP_MAX 16

#define DISCOVERY_NOTIFY_TICK      (1u << 0)
#define DISCOVERY_NOTIFY_TIME_SYNC (1u << 1)
#define DISCOVERY_NOTIFY_STOP      (1u << 2)

typedef enum {
    MICHI_WORKER_STOPPED = 0,
    MICHI_WORKER_RUNNING,
    MICHI_WORKER_STOP_REQUESTED,
    MICHI_WORKER_EXITED,
} michi_worker_lifecycle_t;

static portMUX_TYPE s_lifecycle_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile michi_worker_lifecycle_t s_worker_state = MICHI_WORKER_STOPPED;
#ifdef MICHI_HOST_TEST
static volatile bool s_test_hold_worker = false;
#endif

static bool s_initialized;
static bool s_active;
/* Clock gate (P0-02): the defer warning is logged ONCE per transition
 * into the gated state (never every 30 s tick), and reset as soon as a
 * synchronized announce goes out. */
static bool s_clock_gate_logged;
static char s_ip[MICHI_DISCOVERY_IP_MAX];
static int s_sock = -1;
static bool s_mdns_advertised;
static bool s_server_id_ok;
static char s_server_id[MICHI_DISCOVERY_UUID_LEN];
static esp_timer_handle_t s_announce_timer;
static SemaphoreHandle_t s_announce_mutex;
static SemaphoreHandle_t s_discovery_done_sem;
static TaskHandle_t s_discovery_task;
static uint32_t s_discovery_generation;

/* ------------------------------------------------------------------ */
/* Internals (all called with the announce mutex held)                */
/* ------------------------------------------------------------------ */

static bool identity_ready_locked(void)
{
    if (michi_identity_get_state() != MICHI_IDENTITY_READY) {
        ESP_LOGW(TAG, "discovery: identity not READY (CORRUPT needs a "
                 "factory reset) - announces disabled");
        return false;
    }
    return true;
}

/* Builds the complete signed announce for the current identity/IP and
 * sends ONE datagram to the canonical multicast group. Failures log and
 * degrade the cycle - the timer re-tries. */
static void announce_now_locked(void)
{
    if (!s_server_id_ok || !identity_ready_locked()) {
        return;
    }
    /* P0-02 clock gate: a signed announce carries a timestamp Michi
     * Link rejects outside +-90 s - never emit one until the wall
     * clock is synchronized. The defer warning is logged exactly once
     * per transition (rate-limited, no 30 s spam); the announce
     * resumes immediately via the michi_time sync callback. */
    if (!michi_time_is_synchronized()) {
        if (!s_clock_gate_logged) {
            s_clock_gate_logged = true;
            ESP_LOGW(TAG, "discovery: signed announce deferred, clock "
                     "not synchronized");
        }
        return;
    }
    s_clock_gate_logged = false;
    const michi_product_profile_t *p = michi_product_profile_get();
    if (p == NULL || p->product_name[0] == '\0') {
        /* Boot race: the profile is built later in app_main. The 30 s
         * tick re-tries; nothing is announced with an empty name. */
        ESP_LOGW(TAG, "discovery: profile not ready, announce skipped");
        return;
    }

    char michi_id[MICHI_IDENTITY_MICHI_ID_LEN];
    uint8_t pk_raw[MICHI_IDENTITY_KEY_BYTES];
    char pk_b64[MICHI_IDENTITY_PUBLIC_KEY_B64_LEN];
    if (michi_identity_michi_id(michi_id, sizeof(michi_id)) != ESP_OK ||
        michi_identity_public_key(pk_raw) != ESP_OK ||
        michi_identity_base64url_encode(pk_raw, sizeof(pk_raw), pk_b64,
                                        sizeof(pk_b64)) != ESP_OK) {
        ESP_LOGW(TAG, "discovery: identity material unavailable");
        return;
    }

    /* Fresh 16-byte nonce per announce (replay protection, schema:
     * >= 22 base64url chars). */
    uint8_t nonce_raw[16];
    esp_fill_random(nonce_raw, sizeof(nonce_raw));
    char nonce_b64[32];
    if (michi_identity_base64url_encode(nonce_raw, sizeof(nonce_raw),
                                        nonce_b64, sizeof(nonce_b64)) !=
        ESP_OK) {
        ESP_LOGW(TAG, "discovery: nonce encoding failed");
        return;
    }

    const char *service = (p->tier == MICHI_PRODUCT_HIFI)
                              ? "michi-stream-hifi"
                              : "michi-stream-standard";

    /* Capability flags from the single canonical source
     * (michi_product_profile_capabilities_for): session/heartbeat/volume
     * are advertised true only when audio is available. The announce
     * carries ONLY this canonical group - the extended flags
     * (now_playing/diagnostics/ota) belong to /server/info. */
    const michi_product_capabilities_t caps =
        michi_product_profile_capabilities_for(p);
    const michi_discovery_announce_t announce = {
        .device_id = s_server_id,
        .name = p->product_name,
        .service = service,
        .api_version = MICHI_DISCOVERY_API_VERSION,
        .host = s_ip,
        .port = MICHI_DISCOVERY_HTTP_PORT,
        .feature_session = caps.session,
        .feature_heartbeat = caps.heartbeat,
        .feature_volume = caps.volume,
        .michi_id = michi_id,
        .public_key = pk_b64,
        /* P0-02: the synchronized wall clock (michi_time) - gated
         * above, so this is never a silently invalid 0. */
        .timestamp_ms = michi_time_unix_ms(),
        .nonce = nonce_b64,
    };

    char datagram[MICHI_DISCOVERY_MAX_DATAGRAM_BYTES + 1];
    size_t datagram_len = 0;
    if (michi_discovery_build_announce(&announce, datagram,
                                       sizeof(datagram),
                                       &datagram_len) != ESP_OK) {
        ESP_LOGW(TAG, "discovery: announce build failed");
        return;
    }
    if (s_sock < 0) {
        ESP_LOGW(TAG, "discovery: socket closed, announce skipped");
        return;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = inet_addr(MICHI_DISCOVERY_MULTICAST_GROUP);
    dst.sin_port = htons(MICHI_DISCOVERY_MULTICAST_PORT);
    const int sent = sendto(s_sock, datagram, datagram_len, 0,
                            (struct sockaddr *)&dst, sizeof(dst));
    if (sent != (int)datagram_len) {
        ESP_LOGW(TAG, "discovery: announce sendto failed (errno %d)",
                 errno);
    } else {
        ESP_LOGI(TAG, "discovery: announce sent (%u bytes, ts=%" PRId64 ")",
                 (unsigned)datagram_len, announce.timestamp_ms);
    }
}

/* mDNS TXT: exactly device_id, service, api_version, roles, michi_id
 * (contract 2.2). roles is the PLAIN role string, never JSON. The
 * values are copied by mdns_service_add; the locals must outlive the
 * call only. */
static void advertise_mdns_locked(void)
{
    if (!s_server_id_ok || !identity_ready_locked()) {
        return;
    }
    const michi_product_profile_t *p = michi_product_profile_get();
    if (p == NULL || p->product_name[0] == '\0') {
        ESP_LOGW(TAG, "discovery: profile not ready, mDNS skipped");
        return;
    }
    if (s_mdns_advertised) {
        return;
    }

    char michi_id[MICHI_IDENTITY_MICHI_ID_LEN];
    char service[32];
    char api_version[16];
    if (michi_identity_michi_id(michi_id, sizeof(michi_id)) != ESP_OK) {
        return;
    }
    snprintf(service, sizeof(service), "%s",
             p->tier == MICHI_PRODUCT_HIFI ? "michi-stream-hifi"
                                           : "michi-stream-standard");
    snprintf(api_version, sizeof(api_version), "%s",
             MICHI_DISCOVERY_API_VERSION);

    mdns_txt_item_t txt[] = {
        {"device_id", s_server_id},
        {"service", service},
        {"api_version", api_version},
        {"roles", MICHI_DISCOVERY_ROLE},
        {"michi_id", michi_id},
    };
    const esp_err_t err =
        mdns_service_add(p->product_name, MICHI_DISCOVERY_MDNS_SERVICE,
                         MICHI_DISCOVERY_MDNS_PROTO,
                         MICHI_DISCOVERY_HTTP_PORT, txt,
                         sizeof(txt) / sizeof(txt[0]));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns: service_add failed: %s", esp_err_to_name(err));
        return;
    }
    s_mdns_advertised = true;
    ESP_LOGI(TAG, "mdns: advertising %s.%s (%s) on port %u",
             MICHI_DISCOVERY_MDNS_SERVICE, MICHI_DISCOVERY_MDNS_PROTO,
             p->product_name, (unsigned)MICHI_DISCOVERY_HTTP_PORT);
}

static void retire_mdns_locked(void)
{
    if (!s_mdns_advertised) {
        return;
    }
    s_mdns_advertised = false;
    if (mdns_service_remove(MICHI_DISCOVERY_MDNS_SERVICE,
                            MICHI_DISCOVERY_MDNS_PROTO) != ESP_OK) {
        ESP_LOGW(TAG, "mdns: service_remove failed");
    }
}

static void open_socket_locked(void)
{
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGW(TAG, "discovery: UDP socket failed (errno %d)", errno);
        return;
    }
    /* Link-local datagrams (contract 2.2: IP TTL 1). */
    const int ttl = MICHI_DISCOVERY_UDP_TTL;
    if (setsockopt(s_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl,
                   sizeof(ttl)) != 0) {
        ESP_LOGW(TAG, "discovery: TTL setsockopt failed (errno %d)", errno);
    }
}

/* 30 s +-3 s uniform jitter; restart when already armed (renewal). */
static void arm_announce_timer_locked(void)
{
    if (s_announce_timer == NULL) {
        return;
    }
    const uint32_t span = 2 * MICHI_DISCOVERY_ANNOUNCE_JITTER_MS + 1;
    const int64_t delay_ms =
        (int64_t)MICHI_DISCOVERY_ANNOUNCE_INTERVAL_MS +
        (int64_t)(esp_random() % span) -
        (int64_t)MICHI_DISCOVERY_ANNOUNCE_JITTER_MS;
    const uint64_t delay_us = (uint64_t)delay_ms * 1000;
    esp_err_t err;
    if (esp_timer_is_active(s_announce_timer)) {
        err = esp_timer_restart(s_announce_timer, delay_us);
    } else {
        err = esp_timer_start_once(s_announce_timer, delay_us);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "discovery: announce timer arm failed: %s",
                 esp_err_to_name(err));
    }
}

static void announce_timer_cb(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_lifecycle_mux);
    if (s_worker_state == MICHI_WORKER_RUNNING && s_discovery_task != NULL) {
        xTaskNotify(s_discovery_task, DISCOVERY_NOTIFY_TICK, eSetBits);
    }
    portEXIT_CRITICAL(&s_lifecycle_mux);
}

/* P0-02: michi_time sync callback (runs in the michi_time sync task
 * context). A fresh wall clock resumes the announce IMMEDIATELY -
 * without waiting for the next 30 s tick. Non-blocking notify;
 * ignored when discovery is off. */
static void on_time_sync_cb(void *ctx)
{
    (void)ctx;
    portENTER_CRITICAL(&s_lifecycle_mux);
    if (s_worker_state == MICHI_WORKER_RUNNING && s_discovery_task != NULL) {
        xTaskNotify(s_discovery_task, DISCOVERY_NOTIFY_TIME_SYNC, eSetBits);
    }
    portEXIT_CRITICAL(&s_lifecycle_mux);
}

static void discovery_task_func(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t notified_bits = 0;
        BaseType_t r = xTaskNotifyWait(0, UINT32_MAX, &notified_bits, pdMS_TO_TICKS(50));

        bool stop = false;
        portENTER_CRITICAL(&s_lifecycle_mux);
        if (s_worker_state >= MICHI_WORKER_STOP_REQUESTED ||
            (r == pdTRUE && (notified_bits & DISCOVERY_NOTIFY_STOP))) {
            s_worker_state = MICHI_WORKER_STOP_REQUESTED;
            stop = true;
        }
        portEXIT_CRITICAL(&s_lifecycle_mux);
        if (stop) {
            break;
        }

        bool do_announce = false;
        bool do_rearm = false;

        if (r == pdTRUE) {
            if (notified_bits & DISCOVERY_NOTIFY_TICK) {
                do_announce = true;
                do_rearm = true;
            }
            if (notified_bits & DISCOVERY_NOTIFY_TIME_SYNC) {
                do_announce = true;
            }
        }

        if (do_announce) {
            if (s_worker_state >= MICHI_WORKER_STOP_REQUESTED || s_announce_mutex == NULL) {
                continue;
            }
            if (xSemaphoreTake(s_announce_mutex, pdMS_TO_TICKS(MICHI_DISCOVERY_LOCK_MS)) == pdTRUE) {
                if (s_worker_state == MICHI_WORKER_RUNNING && s_initialized && s_active) {
                    advertise_mdns_locked();
                    announce_now_locked();
                    if (do_rearm) {
                        arm_announce_timer_locked();
                    }
                }
                xSemaphoreGive(s_announce_mutex);
            }
        }
    }

#ifdef MICHI_HOST_TEST
    while (s_test_hold_worker) {
        vTaskDelay(10);
    }
#endif

    portENTER_CRITICAL(&s_lifecycle_mux);
    s_worker_state = MICHI_WORKER_EXITED;
    s_discovery_task = NULL;
    portEXIT_CRITICAL(&s_lifecycle_mux);

    if (s_discovery_done_sem != NULL) {
        xSemaphoreGive(s_discovery_done_sem);
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t michi_discovery_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* Persistent server_id (== device_id). A corrupt store disables the
     * announces (logged) but never regenerates silently - factory reset
     * required (contract section 4). */
    const esp_err_t sid_err = michi_discovery_nvs_get_or_create_server_id(
        s_server_id, sizeof(s_server_id));
    s_server_id_ok = (sid_err == ESP_OK);
    if (sid_err != ESP_OK) {
        ESP_LOGE(TAG, "discovery: server_id unavailable: %s - discovery "
                 "disabled until factory reset",
                 esp_err_to_name(sid_err));
    }

    /* The identity (MS-04) is a hard dependency of the signed announce.
     * No other boot-path owner initializes it yet, so discovery does
     * (idempotent - a future owner can call it again safely). */
    const esp_err_t id_err = michi_identity_init();
    if (id_err != ESP_OK) {
        ESP_LOGE(TAG, "discovery: michi_identity_init failed: %s - no "
                 "signed announces", esp_err_to_name(id_err));
    }

    /* mDNS stack + hostname from the STA MAC (deterministic, valid). */
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "discovery: mdns_init failed: %s (no mDNS)",
                 esp_err_to_name(err));
    } else {
        uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char host[32];
        snprintf(host, sizeof(host), "michi-%02X%02X", mac[4], mac[5]);
        mdns_hostname_set(host);
        ESP_LOGI(TAG, "mdns: hostname=%s", host);
    }

    const esp_timer_create_args_t timer_args = {
        .callback = announce_timer_cb,
        .name = "michi_discovery_announce",
    };
    err = esp_timer_create(&timer_args, &s_announce_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "discovery: announce timer failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    /* P0-02: resume announcing the moment a fresh wall clock lands
     * (michi_time sync callback). Degraded path if the registration
     * fails: the 30 s tick still resumes once synchronized. */
    const esp_err_t cb_err =
        michi_time_register_sync_cb(on_time_sync_cb, NULL);
    if (cb_err != ESP_OK) {
        ESP_LOGW(TAG, "discovery: time sync callback registration "
                 "failed: %s (resume via the periodic tick only)",
                 esp_err_to_name(cb_err));
    }

    s_announce_mutex = xSemaphoreCreateMutex();
    if (s_announce_mutex == NULL) {
        ESP_LOGE(TAG, "discovery: announce mutex failed");
        esp_timer_delete(s_announce_timer);
        s_announce_timer = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_discovery_done_sem = xSemaphoreCreateBinary();
    if (s_discovery_done_sem == NULL) {
        ESP_LOGE(TAG, "discovery: done semaphore failed");
        vSemaphoreDelete(s_announce_mutex);
        s_announce_mutex = NULL;
        esp_timer_delete(s_announce_timer);
        s_announce_timer = NULL;
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_lifecycle_mux);
    s_worker_state = MICHI_WORKER_RUNNING;
    portEXIT_CRITICAL(&s_lifecycle_mux);

    if (xTaskCreate(discovery_task_func, "michi_discovery", 4096, NULL, 5,
                    &s_discovery_task) != pdPASS) {
        ESP_LOGE(TAG, "discovery: task create failed");
        portENTER_CRITICAL(&s_lifecycle_mux);
        s_worker_state = MICHI_WORKER_STOPPED;
        s_discovery_task = NULL;
        portEXIT_CRITICAL(&s_lifecycle_mux);
        vSemaphoreDelete(s_discovery_done_sem);
        s_discovery_done_sem = NULL;
        vSemaphoreDelete(s_announce_mutex);
        s_announce_mutex = NULL;
        esp_timer_delete(s_announce_timer);
        s_announce_timer = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_clock_gate_logged = false;
    s_discovery_generation = 0;
    s_initialized = true;
    ESP_LOGI(TAG, "subsystem=discovery state=ok");
    return ESP_OK;
}

esp_err_t michi_discovery_start(const char *ipv4)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ipv4 == NULL || ipv4[0] == '\0' ||
        strcmp(ipv4, "0.0.0.0") == 0 ||
        strlen(ipv4) >= MICHI_DISCOVERY_IP_MAX) {
        ESP_LOGW(TAG, "discovery: start rejected (invalid IPv4)");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_announce_mutex == NULL ||
        !xSemaphoreTake(s_announce_mutex,
                        pdMS_TO_TICKS(MICHI_DISCOVERY_LOCK_MS))) {
        return ESP_FAIL;
    }

    esp_err_t result = ESP_OK;
    if (!s_server_id_ok || !identity_ready_locked()) {
        result = ESP_ERR_INVALID_STATE;
        goto out;
    }
    if (s_active && strcmp(s_ip, ipv4) != 0) {
        /* IP change: the socket is re-opened bound to the new address
         * (contract 2.2: close/reopen on IP change). */
        open_socket_locked();
        strlcpy(s_ip, ipv4, sizeof(s_ip));
    } else if (!s_active) {
        strlcpy(s_ip, ipv4, sizeof(s_ip));
        open_socket_locked();
    }
    if (s_sock < 0) {
        ESP_LOGW(TAG, "discovery: socket unavailable, announce skipped");
        result = ESP_FAIL;
        goto out;
    }
    s_discovery_generation++;
    s_active = true;

    advertise_mdns_locked();
    announce_now_locked();
    arm_announce_timer_locked();

out:
    xSemaphoreGive(s_announce_mutex);
    return result;
}

esp_err_t michi_discovery_stop(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }
    if (s_announce_mutex == NULL ||
        !xSemaphoreTake(s_announce_mutex,
                        pdMS_TO_TICKS(MICHI_DISCOVERY_LOCK_MS))) {
        return ESP_FAIL;
    }
    if (s_active) {
        s_active = false;
        s_discovery_generation++;
        /* New network-up cycle: a fresh gate transition may log the
         * defer warning again (once per cycle, never per tick). */
        s_clock_gate_logged = false;
        if (s_announce_timer != NULL) {
            esp_timer_stop(s_announce_timer);
        }
        if (s_sock >= 0) {
            close(s_sock);
            s_sock = -1;
        }
        retire_mdns_locked();
        ESP_LOGI(TAG, "subsystem=discovery state=off");
    }
    xSemaphoreGive(s_announce_mutex);
    return ESP_OK;
}

esp_err_t michi_discovery_shutdown(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    /* 1. Unregister external time sync callback immediately so SNTP syncs
     * cannot attempt to access discovery during teardown */
    (void)michi_time_register_sync_cb(NULL, NULL);

    /* 2. Stop announce timer so no new ticks fire */
    s_discovery_generation++;
    if (s_announce_timer != NULL) {
        esp_timer_stop(s_announce_timer);
    }

    /* 3. Request cooperative stop and wake up worker if running */
    portENTER_CRITICAL(&s_lifecycle_mux);
    if (s_worker_state == MICHI_WORKER_RUNNING) {
        s_worker_state = MICHI_WORKER_STOP_REQUESTED;
        if (s_discovery_task != NULL) {
            xTaskNotify(s_discovery_task, DISCOVERY_NOTIFY_STOP, eSetBits);
        }
    }
    portEXIT_CRITICAL(&s_lifecycle_mux);

    /* 4. Join worker: wait for worker to exit */
    if (s_worker_state != MICHI_WORKER_EXITED) {
        if (s_discovery_done_sem != NULL) {
            if (xSemaphoreTake(s_discovery_done_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
                ESP_LOGE(TAG, "discovery: worker join timed out");
                /* On timeout: preserve retriable state, do not destroy resources! */
                return ESP_ERR_TIMEOUT;
            }
        }
    } else {
        if (s_discovery_done_sem != NULL) {
            xSemaphoreTake(s_discovery_done_sem, 0);
        }
    }

    /* 5. Worker is guaranteed dead. Caller is now the sole exclusive owner */
    portENTER_CRITICAL(&s_lifecycle_mux);
    s_worker_state = MICHI_WORKER_STOPPED;
    s_discovery_task = NULL;
    portEXIT_CRITICAL(&s_lifecycle_mux);

    /* 6. Authoritative resource teardown under announce_mutex */
    if (s_announce_mutex != NULL) {
        xSemaphoreTake(s_announce_mutex, portMAX_DELAY);
        if (s_active) {
            s_active = false;
            retire_mdns_locked();
        }
        if (s_sock >= 0) {
            close(s_sock);
            s_sock = -1;
        }
        s_ip[0] = '\0';
        s_clock_gate_logged = false;
        mdns_free();
        if (s_announce_timer != NULL) {
            esp_timer_delete(s_announce_timer);
            s_announce_timer = NULL;
        }
        xSemaphoreGive(s_announce_mutex);
        vSemaphoreDelete(s_announce_mutex);
        s_announce_mutex = NULL;
    } else {
        s_active = false;
        s_ip[0] = '\0';
        s_clock_gate_logged = false;
        mdns_free();
        if (s_announce_timer != NULL) {
            esp_timer_delete(s_announce_timer);
            s_announce_timer = NULL;
        }
    }

    if (s_discovery_done_sem != NULL) {
        vSemaphoreDelete(s_discovery_done_sem);
        s_discovery_done_sem = NULL;
    }
    s_initialized = false;
    ESP_LOGI(TAG, "subsystem=discovery state=off");

    return ESP_OK;
}

esp_err_t michi_discovery_get_server_id(char *out, size_t out_len)
{
    if (out == NULL || out_len < MICHI_DISCOVERY_UUID_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_server_id_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(out, s_server_id, MICHI_DISCOVERY_UUID_LEN);
    return ESP_OK;
}

#ifdef MICHI_HOST_TEST
/* --- test hooks ------------------------------------------------------- */

__attribute__((weak)) void michi_discovery_test_lock(void)
{
    if (s_announce_mutex != NULL) {
        xSemaphoreTake(s_announce_mutex, portMAX_DELAY);
    }
}

__attribute__((weak)) void michi_discovery_test_unlock(void)
{
    if (s_announce_mutex != NULL) {
        xSemaphoreGive(s_announce_mutex);
    }
}

__attribute__((weak)) bool michi_discovery_test_is_timer_active(void)
{
    return s_announce_timer != NULL && esp_timer_is_active(s_announce_timer);
}

__attribute__((weak)) bool michi_discovery_test_is_active(void)
{
    return s_active;
}

__attribute__((weak)) void michi_discovery_test_notify_tick(void)
{
    portENTER_CRITICAL(&s_lifecycle_mux);
    if (s_worker_state == MICHI_WORKER_RUNNING && s_discovery_task != NULL) {
        xTaskNotify(s_discovery_task, DISCOVERY_NOTIFY_TICK, eSetBits);
    }
    portEXIT_CRITICAL(&s_lifecycle_mux);
}

__attribute__((weak)) void michi_discovery_test_hold_worker(bool hold)
{
    s_test_hold_worker = hold;
}

__attribute__((weak)) int michi_discovery_test_worker_state(void)
{
    int st;
    portENTER_CRITICAL(&s_lifecycle_mux);
    st = (int)s_worker_state;
    portEXIT_CRITICAL(&s_lifecycle_mux);
    return st;
}
#endif

