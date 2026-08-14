/*
 * Wi-Fi STA + BLE provisioning (phase 9).
 *
 * Hard rules:
 * - Credentials live ONLY in the NVS "wifi" namespace (ssid, password,
 *   device_name, region). Kconfig has no Wi-Fi credentials (the only
 *   Kconfig secret is the provisioning PoP, MICHI_PROV_POP, which the
 *   protocol requires before the device is on any network). The password
 *   is NEVER logged, not partially: every log carries only the SSID.
 * - esp_event handlers are NON-BLOCKING (P0-11): they post FSM events
 *   and set flags. The reconnect with exponential backoff runs on a
 *   one-shot esp_timer; the blocking BLE provisioning bring-up
 *   (esp_bt_controller_enable) runs in the provisioning task.
 * - The FSM is the single state owner: this component posts the phase-9
 *   events and requests states, it never writes the state directly.
 *
 * Boot flow:
 *   init() reads the "wifi" namespace. Credentials present -> STA start +
 *   connect and boot plan CONNECT; absent -> boot plan PROVISION. The
 *   FSM placement happens in the STATE_CHANGED observer when the FSM
 *   first reaches IDLE (the boot events are posted by app_main AFTER
 *   init(), so at init() time the FSM is still BOOTING):
 *     - CONNECT  : request(WIFI_CONNECTING)
 *     - PROVISION: request(UNPROVISIONED); if the client already
 *                  delivered credentials or the IP before the FSM
 *                  reached IDLE, replay PROVISIONED / NETWORK_READY in
 *                  the right order; otherwise start BLE provisioning.
 *
 * Race safety between the BLE provisioning flow and the FSM placement:
 * CRED_SUCCESS arrives on the esp_event task; GOT_IP may beat the
 * PROVISIONED post. The invariant is centralized in
 * replay_pending_events() (F5): after every transition request it
 * re-reads the s_creds_received / s_network_ready flags and re-posts
 * only the events whose mapping applies to the CURRENT state
 * (PROVISIONED only from UNPROVISIONED, NETWORK_READY only from
 * WIFI_CONNECTING). Already-consumed events are never re-posted, so
 * out-of-contract broadcasts (warn noise) cannot happen.
 *
 * Provisioning (v5.3): wifi_prov_mgr_endpoint_register() requires the
 * provisioning service RUNNING (prov_state between STARTING and
 * STOPPING in manager.c), so the custom endpoint is registered AFTER
 * start_provisioning (same order as the official wifi_prov_mgr example:
 * create -> start -> register, F1). The provisioning task is woken with
 * xTaskNotifyGive for CRED_SUCCESS AND for shutdown/erase; the abort
 * flag (s_shutdown_requested, under the mux) distinguishes a real
 * CRED_SUCCESS wake from a shutdown wake so credentials are only
 * persisted on a real success (F3).
 *
 * Provisioning failures (client timeout, bring-up or NVS persist
 * failure) schedule an automatic service restart on a one-shot
 * esp_timer (MICHI_PROV_RETRY_MS, max 3 attempts, F7); the pairing
 * button long press (MICHI_EVENT_RECOVER) restarts the cycle.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "cJSON.h"

#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

#include "esp_netif_ip_addr.h"
#include "lwip/ip4_addr.h"

#include "michi_discovery.h"
#include "michi_state.h"
#include "michi_time.h"
#include "michi_wifi.h"

#define TAG "michi_wifi"

/* NVS: the ONLY place credentials live. */
#define MICHI_WIFI_NVS_NAMESPACE "wifi"
#define MICHI_WIFI_NVS_KEY_SSID "ssid"
#define MICHI_WIFI_NVS_KEY_PASSWORD "password"
#define MICHI_WIFI_NVS_KEY_DEVICE_NAME "device_name"
#define MICHI_WIFI_NVS_KEY_REGION "region"

#define MICHI_WIFI_SSID_MAX 33
#define MICHI_WIFI_PASS_MAX 65
#define MICHI_WIFI_DEVICE_NAME_MAX 32
#define MICHI_WIFI_REGION_MAX 16

/* Backoff cap: BASE << attempt grows fast (BASE 5000 << 9 ~ 42 min). */
#define MICHI_WIFI_RECONNECT_MAX_MS 300000

/* Automatic provisioning retries after a failed session (F7): the BLE
 * service is restarted after MICHI_PROV_RETRY_MS, max this many retries;
 * exhausted sessions land on UNPROVISIONED (long press restarts). */
#define MICHI_PROV_RETRY_MAX 3

/* Provisioning task: below the FSM (5) and the display render (4) so a
 * blocking BLE bring-up never delays them; above the LED animation (3)
 * and the button debounce (2). */
#define MICHI_WIFI_PROV_TASK_PRIORITY 3

/* Client wait before the provisioning session is torn down. */
#define MICHI_WIFI_PROV_TIMEOUT_MS 600000

/* Cooperative shutdown join timeout for the provisioning task. */
#define MICHI_WIFI_PROV_JOIN_TIMEOUT_MS 2000

typedef enum {
    MICHI_BOOT_PLAN_NONE = 0,
    MICHI_BOOT_PLAN_CONNECT,
    MICHI_BOOT_PLAN_PROVISION,
} michi_boot_plan_t;

/* Device info delivered by the provisioning client (custom endpoint,
 * captured in the protocomm task, consumed by the provisioning task -
 * the xTaskNotifyGive/Take pair orders the accesses). */
typedef struct {
    char device_name[MICHI_WIFI_DEVICE_NAME_MAX];
    char region[MICHI_WIFI_REGION_MAX];
} michi_prov_device_info_t;

static esp_netif_t *s_sta_netif;
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_prov_retry_timer;
static TaskHandle_t s_prov_task;
/* Join target of the provisioning task (shutdown); cleared under the mux
 * so the task never notifies a stale handle. */
static TaskHandle_t s_prov_done_notify;

static volatile bool s_initialized;
static volatile bool s_wifi_started;
static volatile bool s_prov_active;
/* Set under the mux when the provisioning session must NOT persist
 * (shutdown/erase): the task's wake notification is shared between
 * CRED_SUCCESS and the shutdown notify, this flag disambiguates (F3). */
static volatile bool s_shutdown_requested;
static volatile bool s_has_creds;
/* Set when provisioning delivered credentials this boot (before the prov
 * task persists them) - the disconnect/backoff logic must keep working
 * during that window. */
static volatile bool s_creds_received;
/* Set when the STA got an IP this boot (boot-race replay, see file
 * header). */
static volatile bool s_network_ready;
static volatile bool s_plan_applied;
static volatile michi_boot_plan_t s_boot_plan;
static volatile int s_retry;
/* Lifetime (per-boot) reconnect counter (phase 14 diagnostics): incremented
 * under the mux once per armed backoff attempt (arm_reconnect). Unlike
 * s_retry it is NOT reset on GOT_IP: it reports how many reconnects this
 * boot has needed. */
static volatile uint32_t s_reconnects;
/* Provisioning sessions failed this cycle (F7): counts toward
 * MICHI_PROV_RETRY_MAX before the automatic retries are exhausted. */
static volatile int s_prov_retries;

/* SSID cache (log/UI only, NEVER the password). Written under the mux;
 * readers may observe the previous value - the cache is not a
 * synchronization channel. */
static char s_ssid_cache[MICHI_WIFI_SSID_MAX];

static michi_prov_device_info_t s_prov_device_info;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* ------------------------------------------------------------------ */
/* NVS "wifi" namespace                                                */
/* ------------------------------------------------------------------ */

static esp_err_t wifi_nvs_open(nvs_handle_t *out, nvs_open_mode_t mode)
{
    return nvs_open(MICHI_WIFI_NVS_NAMESPACE, mode, out);
}

/* Loads ssid + password; returns true when BOTH exist. */
static bool wifi_nvs_load_creds(char *ssid, size_t ssid_len,
                                char *password, size_t pass_len)
{
    nvs_handle_t h;
    if (wifi_nvs_open(&h, NVS_READONLY) != ESP_OK) {
        return false;
    }
    size_t len = ssid_len;
    esp_err_t err = nvs_get_str(h, MICHI_WIFI_NVS_KEY_SSID, ssid, &len);
    if (err == ESP_OK) {
        len = pass_len;
        err = nvs_get_str(h, MICHI_WIFI_NVS_KEY_PASSWORD, password, &len);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ssid[0] = '\0';
        password[0] = '\0';
        return false;
    }
    return true;
}

static esp_err_t wifi_nvs_store_creds(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = wifi_nvs_open(&h, NVS_READWRITE);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, MICHI_WIFI_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, MICHI_WIFI_NVS_KEY_PASSWORD, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t wifi_nvs_store_device_info(const michi_prov_device_info_t *info)
{
    nvs_handle_t h;
    esp_err_t err = wifi_nvs_open(&h, NVS_READWRITE);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, MICHI_WIFI_NVS_KEY_DEVICE_NAME, info->device_name);
    if (err == ESP_OK) {
        err = nvs_set_str(h, MICHI_WIFI_NVS_KEY_REGION, info->region);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t wifi_nvs_erase_all(void)
{
    nvs_handle_t h;
    esp_err_t err = wifi_nvs_open(&h, NVS_READWRITE);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ------------------------------------------------------------------ */
/* Reconnect with exponential backoff (esp_timer one-shot, P0-11)      */
/* ------------------------------------------------------------------ */

/* The FSM is already WIFI_CONNECTING when this fires (the DISCONNECTED
 * event mapped IDLE -> WIFI_CONNECTING when the retry was scheduled);
 * nudge it only when the post was dropped. */
static void fail_retry_chain(int retry);
static void arm_reconnect(void);

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (!s_initialized) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    const bool creds = s_has_creds || s_creds_received;
    const int retry = s_retry;
    portEXIT_CRITICAL(&s_mux);
    if (!creds) {
        ESP_LOGW(TAG, "wifi: reconnect cancelled (no credentials)");
        return;
    }
    ESP_LOGI(TAG, "wifi: state=connecting retry=%u", (unsigned)retry);
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi: esp_wifi_connect failed: %s",
                 esp_err_to_name(err));
        /* F6(a)/F6(c): liveness - without a DISCONNECTED event (e.g. the
         * driver rejects the connect) the chain must not die silently:
         * the backoff timer covers connect failures too. */
        if (retry >= CONFIG_MICHI_WIFI_RETRY_MAX) {
            fail_retry_chain(retry);
        } else {
            arm_reconnect();
        }
    } else if (michi_state_get() != MICHI_STATE_WIFI_CONNECTING) {
        michi_state_request(MICHI_STATE_WIFI_CONNECTING);
    }
}

/* Retry chain exhausted: broadcast the error and land the FSM on
 * RECOVERABLE_ERROR. When the FSM is still BOOTING/SELF_TEST the request
 * is dropped (no transition) - the STATE_CHANGED observer re-arms the
 * chain when WIFI_CONNECTING is reached with no pending attempt (F2(c)). */
static void fail_retry_chain(int retry)
{
    ESP_LOGE(TAG, "subsystem=wifi state=failed phase=9 retry=%u",
             (unsigned)retry);
    /* F15: report_error captures the cause directly (guaranteed even with
     * a full bus queue) and posts best-effort for the observers. */
    (void)michi_state_report_error(MICHI_EVENT_ERROR,
                                   ESP_ERR_WIFI_NOT_CONNECT);
    michi_state_request(MICHI_STATE_RECOVERABLE_ERROR);
}

/* Arms the next backoff attempt (F14: esp_timer_restart when the timer is
 * already armed, start_once otherwise - restart alone returns
 * ESP_ERR_INVALID_STATE on a disarmed timer; s_retry increments ONLY when
 * the arm succeeded). */
static void arm_reconnect(void)
{
    portENTER_CRITICAL(&s_mux);
    const int retry = s_retry;
    portEXIT_CRITICAL(&s_mux);

    if (retry >= CONFIG_MICHI_WIFI_RETRY_MAX) {
        fail_retry_chain(retry);
        return;
    }

    int64_t delay_ms = (int64_t)CONFIG_MICHI_WIFI_RECONNECT_BASE_MS << retry;
    if (delay_ms > MICHI_WIFI_RECONNECT_MAX_MS) {
        delay_ms = MICHI_WIFI_RECONNECT_MAX_MS;
    }
    const uint64_t delay_us = (uint64_t)delay_ms * 1000;
    esp_err_t err;
    if (esp_timer_is_active(s_reconnect_timer)) {
        err = esp_timer_restart(s_reconnect_timer, delay_us);
    } else {
        err = esp_timer_start_once(s_reconnect_timer, delay_us);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi: reconnect timer arm failed: %s",
                 esp_err_to_name(err));
        return;
    }
    ESP_LOGW(TAG, "wifi: ssid=%s retry=%u backoff_ms=%" PRId64,
             s_ssid_cache, (unsigned)retry, delay_ms);
    portENTER_CRITICAL(&s_mux);
    s_retry = retry + 1;
    /* F14: one reconnect per armed backoff attempt (monotonic, see the
     * s_reconnects declaration). */
    s_reconnects++;
    portEXIT_CRITICAL(&s_mux);
}

/* ------------------------------------------------------------------ */
/* Signed discovery (MS-05)                                           */
/* ------------------------------------------------------------------ */

/* mDNS + the UDP announce are owned by michi_discovery: the
 * service/TXT/group/port constants and the announce timer live there.
 * michi_wifi only notifies network up/down - no duplicated product
 * strings, no duplicated announce state. */

/* ------------------------------------------------------------------ */
/* esp_event handlers (NON-BLOCKING, P0-11)                            */
/* ------------------------------------------------------------------ */

/* F5: re-reads the boot-race flags AFTER a transition request/event and
 * re-posts only the events whose mapping applies to the CURRENT state:
 * WIFI_PROVISIONED only from UNPROVISIONED, NETWORK_READY only from
 * WIFI_CONNECTING. Already-consumed events are never re-posted, so
 * out-of-contract broadcasts (warn noise) cannot happen. Call it after
 * every transition request and from the STATE_CHANGED observer. */
static void replay_pending_events(void)
{
    portENTER_CRITICAL(&s_mux);
    const bool creds = s_creds_received;
    const bool ready = s_network_ready;
    portEXIT_CRITICAL(&s_mux);
    const michi_state_t st = michi_state_get();
    if (creds && st == MICHI_STATE_UNPROVISIONED) {
        michi_state_post(MICHI_EVENT_WIFI_PROVISIONED, 0);
    }
    if (ready && st == MICHI_STATE_WIFI_CONNECTING) {
        michi_state_post(MICHI_EVENT_NETWORK_READY, 0);
    }
}

static void handle_got_ip(const ip_event_got_ip_t *event)
{
    if (!s_initialized) {
        /* Shutdown in progress: never move the FSM or re-advertise. */
        return;
    }
    char ipbuf[IP4ADDR_STRLEN_MAX] = "0.0.0.0";
    if (event != NULL) {
        ip4addr_ntoa_r((const ip4_addr_t *)&event->ip_info.ip, ipbuf,
                       sizeof(ipbuf));
    }

    portENTER_CRITICAL(&s_mux);
    s_retry = 0;
    s_network_ready = true;
    portEXIT_CRITICAL(&s_mux);

    ESP_LOGI(TAG, "subsystem=wifi state=connected phase=9");
    ESP_LOGI(TAG, "wifi: ssid=%s ip=%s retry=0", michi_wifi_get_ssid(),
             ipbuf);

    /* Signed discovery (MS-05): mDNS service + first UDP announce on
     * network up; IP renewals re-announce with the current address. */
    const esp_err_t d_err = michi_discovery_start(ipbuf);
    if (d_err != ESP_OK) {
        ESP_LOGW(TAG, "discovery: announce start failed: %s",
                 esp_err_to_name(d_err));
    }

    /* Wall clock (P0-02): start/restart SNTP only once the STA has an
     * IP; the signed announce stays gated until a fresh sync lands
     * (michi_time sync callback resumes it immediately). */
    const esp_err_t t_err = michi_time_start();
    if (t_err != ESP_OK) {
        ESP_LOGW(TAG, "time: sync start failed: %s (signed announces "
                 "stay gated)", esp_err_to_name(t_err));
    }

    /* F5: replay only the events whose mapping applies to the CURRENT
     * state (no warn noise, e.g. on IP renewal while IDLE). When the FSM
     * is still BOOTING/SELF_TEST nothing is posted here - the
     * STATE_CHANGED observer replays when the FSM lands; when the
     * provisioning client beat the FSM (state UNPROVISIONED) PROVISIONED
     * is queued and the observer posts NETWORK_READY once WIFI_CONNECTING
     * applies. */
    replay_pending_events();
}

static void handle_disconnected(void)
{
    if (!s_initialized) {
        /* Shutdown in progress (esp_wifi_disconnect fires this): never
         * re-arm the chain or move the FSM. */
        return;
    }
    /* Signed discovery: close the UDP socket, stop the announce timer
     * and retire the mDNS service on link loss (contract 2.2). */
    (void)michi_discovery_stop();

    /* Wall clock (P0-02): conserve the last sync state on link loss
     * (documented policy in michi_time.h); the next GOT_IP revalidates
     * with a fresh SNTP sync. */
    (void)michi_time_stop();

    portENTER_CRITICAL(&s_mux);
    const bool prov_active = s_prov_active;
    portEXIT_CRITICAL(&s_mux);

    if (prov_active) {
        /* F6(b): the provisioning flow owns the connect: wifi_prov_mgr
         * retries and the client can re-send credentials - the FSM is in
         * UNPROVISIONED/WIFI_CONNECTING, never in the IDLE that the
         * DISCONNECTED mapping expects. Once the session ends,
         * prov_task_finish arms the first reconnect attempt when the STA
         * is not connected. */
        ESP_LOGD(TAG, "wifi: disconnected during provisioning "
                 "(connect owned by the provisioning flow)");
        return;
    }
    if (!michi_wifi_is_provisioned()) {
        ESP_LOGW(TAG, "wifi: disconnected, no credentials");
        return;
    }
    ESP_LOGW(TAG, "wifi: ssid=%s state=disconnected",
             michi_wifi_get_ssid());

    /* FSM: IDLE -> WIFI_CONNECTING only when the device was in IDLE; the
     * retry attempts happen while already WIFI_CONNECTING (no event). */
    if (michi_state_get() == MICHI_STATE_IDLE) {
        michi_state_post(MICHI_EVENT_WIFI_DISCONNECTED, 0);
    }
    /* F6(c): when a timer is already armed (e.g. the init connect failed
     * and this DISCONNECTED event raced the init arm), the pending
     * attempt covers this disconnect - do not double-arm. */
    if (s_reconnect_timer != NULL &&
        !esp_timer_is_active(s_reconnect_timer)) {
        arm_reconnect();
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id,
                          void *data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGD(TAG, "wifi: STA started");
            break;
        case WIFI_EVENT_STA_CONNECTED:
            /* L2 link up: broadcast only (observers see it); the FSM
             * moves on NETWORK_READY (IP). */
            ESP_LOGI(TAG, "wifi: ssid=%s state=connected_l2",
                     michi_wifi_get_ssid());
            michi_state_post(MICHI_EVENT_WIFI_CONNECTED, 0);
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *ev =
                (const wifi_event_sta_disconnected_t *)data;
            ESP_LOGW(TAG, "wifi: disconnected reason=%d",
                     ev != NULL ? (int)ev->reason : -1);
            handle_disconnected();
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        handle_got_ip((const ip_event_got_ip_t *)data);
    }
}

/* ------------------------------------------------------------------ */
/* BLE provisioning (wifi_prov_mgr, NimBLE)                           */
/* ------------------------------------------------------------------ */

/* Runs on the wifi_prov_mgr event dispatch (event loop task context):
 * NON-BLOCKING (P0-11) - posts FSM events, sets flags, notifies the
 * provisioning task. Event semantics in v5.3 (wifi_prov_cb_event_t):
 * CRED_RECV = credentials received and applied to the STA (connect is
 * scheduled by the manager); CRED_SUCCESS = the device CONNECTED to the
 * AP (only now are the credentials proven good); CRED_FAIL = connect
 * failed (the manager keeps the session open, the client can retry). */
static void prov_event_cb(void *user_data, wifi_prov_cb_event_t event,
                          void *event_data)
{
    (void)user_data;
    (void)event_data;

    switch (event) {
    case WIFI_PROV_CRED_RECV:
        ESP_LOGI(TAG, "provisioning: credentials received");
        portENTER_CRITICAL(&s_mux);
        s_creds_received = true;
        portEXIT_CRITICAL(&s_mux);
        /* Early FSM move: UNPROVISIONED -> WIFI_CONNECTING while the
         * manager connects. F5: the replay gates the post on the current
         * state (from WIFI_CONNECTING/IDLE the event is broadcast-only,
         * the boot observer or GOT_IP already moved the FSM). */
        replay_pending_events();
        break;
    case WIFI_PROV_CRED_SUCCESS: {
        ESP_LOGI(TAG, "subsystem=wifi state=provisioned phase=9");
        portENTER_CRITICAL(&s_mux);
        const bool active = s_prov_active;
        portEXIT_CRITICAL(&s_mux);
        if (active) {
            /* The provisioning task persists the proven credentials.
             * F3: shutdown/erase notify the same task - the abort flag
             * (s_shutdown_requested) decides whether this wake may
             * persist. */
            xTaskNotifyGive(s_prov_task);
        }
        break;
    }
    case WIFI_PROV_CRED_FAIL:
        ESP_LOGW(TAG, "subsystem=wifi state=provisioning_failed phase=9 "
                 "(client can retry)");
        break;
    default:
        break;
    }
}

/* Custom endpoint handler (protocomm task context): captures
 * {"device_name": ..., "region": ...}. Copies EVERY value into the local
 * buffer before cJSON_Delete (no pointers past the tree), same contract
 * as the HTTP layer (phase 4). */
static esp_err_t device_info_handler(uint32_t session_id,
                                     const uint8_t *inbuf, ssize_t inlen,
                                     uint8_t **outbuf, ssize_t *outlen,
                                     void *priv_data)
{
    (void)session_id;
    (void)priv_data;
    *outbuf = NULL;
    *outlen = 0;

    if (inbuf == NULL || inlen <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char *body = strndup((const char *)inbuf, (size_t)inlen);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    michi_prov_device_info_t info = {0};
    const cJSON *n = cJSON_GetObjectItem(root, "device_name");
    const cJSON *r = cJSON_GetObjectItem(root, "region");
    if (n != NULL && cJSON_IsString(n) && n->valuestring[0] != '\0') {
        snprintf(info.device_name, sizeof(info.device_name), "%s",
                 n->valuestring);
    }
    if (r != NULL && cJSON_IsString(r) && r->valuestring[0] != '\0') {
        snprintf(info.region, sizeof(info.region), "%s", r->valuestring);
    }
    cJSON_Delete(root);

    if (info.device_name[0] == '\0' && info.region[0] == '\0') {
        ESP_LOGW(TAG, "provisioning: device-info endpoint got no usable "
                 "fields");
    } else {
        /* protocomm task writes, provisioning task reads after the task
         * notification (ordered by the FreeRTOS notify pair). */
        memcpy(&s_prov_device_info, &info, sizeof(info));
    }

    static const char ok[] = "SUCCESS";
    *outbuf = (uint8_t *)strdup(ok);
    if (*outbuf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* +1 for the NUL byte, same convention as the official example. */
    *outlen = (ssize_t)strlen(ok) + 1;
    return ESP_OK;
}

/* Runs when the provisioning flow ends (task context):
 * - success (real CRED_SUCCESS wake, F3): the credentials are PROVEN good
 *   (the device connected) - persist them + the captured device info, and
 *   cover the boot race (F5 replay).
 * - failure: client timeout or persist failure - if the FSM was
 *   mid-connect (WIFI_CONNECTING, the early PROVISIONED post already moved
 *   it), PROV_FAILED drives WIFI_CONNECTING -> UNPROVISIONED (F8); the
 *   automatic retry (F7) is scheduled by prov_task.
 *
 * @return true when the credentials were persisted (device provisioned).
 */
static bool prov_task_finish(bool success)
{
    if (!success) {
        if (s_initialized &&
            michi_state_get() == MICHI_STATE_WIFI_CONNECTING) {
            michi_state_post(MICHI_EVENT_WIFI_PROV_FAILED, 0);
        }
        return false;
    }

    wifi_config_t wifi_cfg;
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    if (esp_wifi_get_config(ESP_IF_WIFI_STA, &wifi_cfg) == ESP_OK &&
        wifi_cfg.sta.ssid[0] != '\0') {
        const esp_err_t err =
            wifi_nvs_store_creds((const char *)wifi_cfg.sta.ssid,
                                 (const char *)wifi_cfg.sta.password);
        if (err == ESP_OK) {
            portENTER_CRITICAL(&s_mux);
            s_has_creds = true;
            /* F12: the SSID cache is written under the mux (readers
             * already read it under the mux). */
            snprintf(s_ssid_cache, sizeof(s_ssid_cache), "%s",
                     (const char *)wifi_cfg.sta.ssid);
            portEXIT_CRITICAL(&s_mux);
            if (s_prov_device_info.device_name[0] != '\0' ||
                s_prov_device_info.region[0] != '\0') {
                wifi_nvs_store_device_info(&s_prov_device_info);
            }
            ESP_LOGI(TAG, "wifi: credentials stored (ssid=%s)",
                     s_ssid_cache);
        } else {
            /* F8: an NVS persist failure is a REAL failure (the "wifi"
             * namespace shares the 24 KB nvs partition): the credentials
             * are NOT marked applied, the FSM lands on UNPROVISIONED and
             * the automatic retry (F7) tries again. */
            ESP_LOGE(TAG, "provisioning: persist failed (%s) - "
                     "credentials NOT marked as applied",
                     esp_err_to_name(err));
            if (s_initialized &&
                michi_state_get() == MICHI_STATE_WIFI_CONNECTING) {
                michi_state_post(MICHI_EVENT_WIFI_PROV_FAILED, 0);
            }
            return false;
        }
    } else {
        ESP_LOGE(TAG, "provisioning: no station config to persist");
        return false;
    }

    /* Boot race cover (F5): the FSM may still be UNPROVISIONED (the early
     * CRED_RECV post raced the FSM placement) - replay only what still
     * applies. */
    replay_pending_events();

    /* F6(b): a disconnect landed in the CRED_SUCCESS -> persist window
     * (handle_disconnected early-returns while the session is active) and
     * the manager is being torn down; arm the first reconnect attempt
     * when the STA is not connected. */
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        portENTER_CRITICAL(&s_mux);
        s_retry = 0;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "wifi: not connected after provisioning session, "
                 "arming first attempt");
        arm_reconnect();
    }
    return true;
}

/* F7: automatic provisioning retry. Called by prov_task after a session
 * that did not end provisioned (bring-up failure, client timeout, NVS
 * persist failure) - NOT after a shutdown/erase abort. Up to
 * MICHI_PROV_RETRY_MAX retries, then a clear log and UNPROVISIONED (the
 * pairing button long press restarts the cycle; phase 10 coordinates the
 * pairing flow when it lands). */
static void schedule_prov_retry(void)
{
    portENTER_CRITICAL(&s_mux);
    s_prov_retries++;
    const int attempts = s_prov_retries;
    const bool init = s_initialized;
    portEXIT_CRITICAL(&s_mux);
    if (!init) {
        return;
    }
    if (attempts >= MICHI_PROV_RETRY_MAX) {
        ESP_LOGE(TAG, "provisioning: automatic retries exhausted (%d) - "
                 "long press the pairing button to restart the cycle "
                 "(phase-10 pairing will coordinate)",
                 attempts);
        const michi_state_t st = michi_state_get();
        if (st == MICHI_STATE_IDLE) {
            michi_state_request(MICHI_STATE_UNPROVISIONED);
        } else if (st == MICHI_STATE_WIFI_CONNECTING) {
            michi_state_post(MICHI_EVENT_WIFI_PROV_FAILED, 0);
        }
        return;
    }
    ESP_LOGW(TAG, "provisioning: automatic retry %d/%d in %d ms",
             attempts, MICHI_PROV_RETRY_MAX, CONFIG_MICHI_PROV_RETRY_MS);
    if (s_prov_retry_timer == NULL) {
        return;
    }
    esp_err_t err;
    if (esp_timer_is_active(s_prov_retry_timer)) {
        err = esp_timer_restart(s_prov_retry_timer,
                                (uint64_t)CONFIG_MICHI_PROV_RETRY_MS * 1000);
    } else {
        err = esp_timer_start_once(s_prov_retry_timer,
                                   (uint64_t)CONFIG_MICHI_PROV_RETRY_MS * 1000);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "provisioning: retry timer arm failed: %s",
                 esp_err_to_name(err));
    }
}

static void prov_retry_timer_cb(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_mux);
    const bool init = s_initialized;
    const bool creds = s_has_creds;
    const bool active = s_prov_active;
    portEXIT_CRITICAL(&s_mux);
    if (!init || creds || active) {
        return;
    }
    ESP_LOGI(TAG, "provisioning: automatic retry starting");
    const esp_err_t err = michi_wifi_start_provisioning();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "provisioning: automatic retry start failed: %s",
                 esp_err_to_name(err));
        /* Liveness: a failed start consumes no session - keep the retry
         * chain alive (it counts down and exhausts). */
        schedule_prov_retry();
    }
}

/* Stops an active provisioning session and joins the provisioning task
 * (cooperative, same pattern as the button). Marks the session as aborted
 * (F3) so the task never persists credentials on the wake. Returns true
 * when the task exited, false on join timeout. */
static bool stop_provisioning_join(void)
{
    portENTER_CRITICAL(&s_mux);
    s_prov_done_notify = xTaskGetCurrentTaskHandle();
    s_shutdown_requested = true;
    portEXIT_CRITICAL(&s_mux);
    /* v5.3: wifi_prov_mgr_stop_provisioning() returns void. */
    wifi_prov_mgr_stop_provisioning();
    xTaskNotifyGive(s_prov_task);
    if (ulTaskNotifyTake(pdFALSE,
                         pdMS_TO_TICKS(MICHI_WIFI_PROV_JOIN_TIMEOUT_MS)) == 0) {
        portENTER_CRITICAL(&s_mux);
        s_prov_done_notify = NULL;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "provisioning: task did not stop in %d ms",
                 (int)MICHI_WIFI_PROV_JOIN_TIMEOUT_MS);
        return false;
    }
    return true;
}

static void prov_task(void *arg)
{
    (void)arg;

    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
        .app_event_handler = {
            .event_cb = prov_event_cb,
            .user_data = NULL,
        },
    };

    esp_err_t err = wifi_prov_mgr_init(config);
    char service_name[64] = "";
    if (err == ESP_OK) {
        /* F1: v5.3 wifi_prov_mgr_endpoint_register() requires the
         * provisioning service RUNNING (prov_state between STARTING and
         * STOPPING in manager.c) - registering before start_provisioning
         * returns ESP_FAIL. Same order as the official v5.3 wifi_prov_mgr
         * example: endpoint_create -> start_provisioning ->
         * endpoint_register. */
        err = wifi_prov_mgr_endpoint_create("michi-device-info");
        if (err == ESP_OK) {
            uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
            esp_read_mac(mac, ESP_MAC_WIFI_STA);
            snprintf(service_name, sizeof(service_name), "%s%02X%02X",
                     CONFIG_MICHI_PROV_SERVICE_NAME_PREFIX, mac[4], mac[5]);
            if (strlen(service_name) > 31) {
                ESP_LOGW(TAG, "provisioning: service name exceeds the BLE "
                         "31-byte limit: %s", service_name);
            }
            err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1,
                                                   CONFIG_MICHI_PROV_POP,
                                                   service_name, NULL);
        }
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "subsystem=wifi state=provisioning phase=9 "
                     "service=%s", service_name);
            err = wifi_prov_mgr_endpoint_register("michi-device-info",
                                                  device_info_handler,
                                                  NULL);
            if (err != ESP_OK) {
                /* F1: post-start register failure: stop the session and
                 * propagate as a bring-up failure (retry via F7). */
                ESP_LOGE(TAG, "provisioning: custom endpoint register "
                         "failed: %s", esp_err_to_name(err));
                wifi_prov_mgr_stop_provisioning();
            }
        }
    }

    bool provisioned = false;
    bool abort = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "provisioning: bring-up failed: %s",
                 esp_err_to_name(err));
    } else {
        /* Wait for the CRED_SUCCESS notify (prov_event_cb) or for the
         * client to walk away. F3: shutdown/erase also notify this task
         * (stop_provisioning_join) - the abort flag set under the mux
         * distinguishes a real CRED_SUCCESS wake from a shutdown wake. */
        const uint32_t waited =
            ulTaskNotifyTake(pdFALSE,
                             pdMS_TO_TICKS(MICHI_WIFI_PROV_TIMEOUT_MS));
        if (waited == 0) {
            ESP_LOGW(TAG, "provisioning: client wait timed out");
        }
        portENTER_CRITICAL(&s_mux);
        const bool active = s_prov_active;
        abort = s_shutdown_requested;
        portEXIT_CRITICAL(&s_mux);
        if (active) {
            provisioned = prov_task_finish(waited != 0 && !abort);
        }
    }

    portENTER_CRITICAL(&s_mux);
    s_prov_active = false;
    portEXIT_CRITICAL(&s_mux);

    /* Always: stop the manager (auto-stop may still be pending) and
     * release the BLE transport. */
    wifi_prov_mgr_deinit();

    /* Cooperative join (same pattern as the button): notify under the
     * mux so a shutdown caller that already timed out and cleared the
     * target can never be hit with a stale handle. */
    portENTER_CRITICAL(&s_mux);
    if (s_prov_done_notify != NULL) {
        xTaskNotifyGive(s_prov_done_notify);
        s_prov_done_notify = NULL;
    }
    portEXIT_CRITICAL(&s_mux);

    /* F7: a session that did not end provisioned (bring-up failure,
     * client timeout, persist failure) schedules the automatic retry -
     * a shutdown/erase abort never does. */
    if (!provisioned && !abort) {
        schedule_prov_retry();
    }

    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/* Boot placement observer: acts when the FSM first reaches IDLE (from
 * SELF_TEST) and re-arms the retry chain when WIFI_CONNECTING is reached
 * with no pending attempt (F2(c)). Runs on the FSM task: only posts
 * requests/events and spawns the provisioning task - no blocking. */
static void state_observer(const michi_event_t *ev)
{
    if (ev->id != MICHI_EVENT_STATE_CHANGED) {
        return;
    }
    const michi_state_t target = (michi_state_t)ev->data;

    if (target == MICHI_STATE_IDLE) {
        portENTER_CRITICAL(&s_mux);
        const bool already = s_plan_applied;
        s_plan_applied = true;
        const michi_boot_plan_t plan = s_boot_plan;
        const bool creds = s_creds_received;
        portEXIT_CRITICAL(&s_mux);
        if (already) {
            return;
        }

        if (plan == MICHI_BOOT_PLAN_CONNECT) {
            michi_state_request(MICHI_STATE_WIFI_CONNECTING);
        } else {
            /* PLAN_PROVISION: land on UNPROVISIONED, replay any events the
             * client already caused (in order), otherwise start BLE
             * provisioning. */
            michi_state_request(MICHI_STATE_UNPROVISIONED);
            if (!creds) {
                const esp_err_t err = michi_wifi_start_provisioning();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "boot: automatic provisioning start "
                             "failed: %s", esp_err_to_name(err));
                }
            }
        }
    } else if (target == MICHI_STATE_WIFI_CONNECTING) {
        /* F2(c): when the retry chain exhausted while the FSM was still
         * BOOTING/SELF_TEST, the RECOVERABLE_ERROR request was dropped
         * (no transition) and nothing would ever connect again - re-arm
         * a fresh chain (no pending attempt = no active timer). */
        portENTER_CRITICAL(&s_mux);
        const int retry = s_retry;
        portEXIT_CRITICAL(&s_mux);
        if (retry >= CONFIG_MICHI_WIFI_RETRY_MAX &&
            s_reconnect_timer != NULL &&
            !esp_timer_is_active(s_reconnect_timer)) {
            ESP_LOGW(TAG, "wifi: retry chain exhausted before the FSM "
                     "settled, re-arming from retry=0");
            portENTER_CRITICAL(&s_mux);
            s_retry = 0;
            portEXIT_CRITICAL(&s_mux);
            arm_reconnect();
        }
    }

    /* F5: re-read the flags after the transition request and re-post only
     * the events that still apply (e.g. NETWORK_READY once PROVISIONED
     * moved the FSM to WIFI_CONNECTING). */
    replay_pending_events();
}

/* F2(a): MICHI_EVENT_RECOVER (pairing button long press, recovery
 * action): reset the retry counter and re-arm the connection attempt. The
 * FSM maps RECOVERABLE_ERROR -> IDLE for this event AFTER the observers
 * run, so the request is validated from either state (both are in the
 * transition table). Runs on the FSM task: esp_wifi_connect/get_config
 * dispatch to the wifi task and return without waiting for the outcome. */
static void recover_observer(const michi_event_t *ev)
{
    if (ev->id != MICHI_EVENT_RECOVER) {
        return;
    }

    portENTER_CRITICAL(&s_mux);
    s_retry = 0;
    const bool creds = s_has_creds;
    portEXIT_CRITICAL(&s_mux);

    if (!creds) {
        /* Unprovisioned recovery: restart the provisioning cycle (F7).
         * Place the FSM first (dispatch-time state is RECOVERABLE_ERROR,
         * the RECOVER mapping applies IDLE right after - the request
         * lands valid from either). */
        const michi_state_t st0 = michi_state_get();
        if (st0 == MICHI_STATE_IDLE ||
            st0 == MICHI_STATE_RECOVERABLE_ERROR) {
            michi_state_request(MICHI_STATE_UNPROVISIONED);
        }
        ESP_LOGI(TAG, "wifi: recovery with no credentials, restarting "
                 "provisioning");
        portENTER_CRITICAL(&s_mux);
        s_prov_retries = 0;
        portEXIT_CRITICAL(&s_mux);
        const esp_err_t err = michi_wifi_start_provisioning();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wifi: recovery provisioning start failed: %s",
                     esp_err_to_name(err));
        }
        return;
    }

    /* STA configured? esp_wifi_connect requires a valid config. */
    wifi_config_t wifi_cfg;
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    if (esp_wifi_get_config(ESP_IF_WIFI_STA, &wifi_cfg) != ESP_OK ||
        wifi_cfg.sta.ssid[0] == '\0') {
        ESP_LOGW(TAG, "wifi: recovery skipped (no STA config)");
        return;
    }
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi: recovery connect failed: %s",
                 esp_err_to_name(err));
        /* Liveness: without a DISCONNECTED event the chain would die
         * silent - the backoff timer covers the connect failure. */
        arm_reconnect();
    }

    /* Dispatch-time state is RECOVERABLE_ERROR (the mapping applies
     * after the observers); the request lands once RECOVERABLE_ERROR ->
     * IDLE applied. */
    const michi_state_t st = michi_state_get();
    if (st == MICHI_STATE_IDLE ||
        st == MICHI_STATE_RECOVERABLE_ERROR) {
        michi_state_request(MICHI_STATE_WIFI_CONNECTING);
    }
    replay_pending_events();
}

esp_err_t michi_wifi_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "init: esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "init: event loop failed: %s", esp_err_to_name(err));
        return err;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        ESP_LOGE(TAG, "init: STA netif creation failed");
        return ESP_FAIL;
    }

    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init: esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    esp_wifi_set_mode(WIFI_MODE_STA);
    /* F11: handler registration errors propagate like every other init
     * error (F9) - a missing handler would silently drop every network
     * event. */
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init: WIFI_EVENT handler register failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                     event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init: IP_EVENT handler register failed: %s",
                 esp_err_to_name(err));
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     event_handler);
        return err;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .name = "michi_wifi_reconnect",
    };
    err = esp_timer_create(&timer_args, &s_reconnect_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init: reconnect timer failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    const esp_timer_create_args_t retry_timer_args = {
        .callback = prov_retry_timer_cb,
        .name = "michi_wifi_prov_retry",
    };
    err = esp_timer_create(&retry_timer_args, &s_prov_retry_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init: provisioning retry timer failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    /* Wall clock (P0-02): SNTP config + sync task. Initialized BEFORE
     * michi_discovery so the discovery sync-callback registration has
     * a live time subsystem. A failure is logged and the signed
     * announce stays gated - the rest of the firmware runs unaffected. */
    err = michi_time_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "init: michi_time_init failed: %s (signed "
                 "announces stay gated)", esp_err_to_name(err));
    }

    /* Signed discovery (MS-05): owned by michi_discovery - mDNS stack,
     * announce timer and UDP socket. Runs here (not in app_main) so the
     * network bring-up and the announce lifecycle share one place; the
     * service itself is advertised on GOT_IP. */
    err = michi_discovery_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "init: michi_discovery_init failed: %s "
                 "(no discovery announcements)", esp_err_to_name(err));
    }

    /* Credentials: NVS "wifi" namespace ONLY. */
    char password[MICHI_WIFI_PASS_MAX] = {0};
    const bool have_creds = wifi_nvs_load_creds(s_ssid_cache,
                                                sizeof(s_ssid_cache),
                                                password, sizeof(password));
    if (have_creds) {
        portENTER_CRITICAL(&s_mux);
        s_has_creds = true;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "wifi: stored credentials (ssid=%s)", s_ssid_cache);
    }

    /* Boot plan. */
    if (have_creds) {
        /* F9: esp_wifi_start/set_config failures are REAL init failures -
         * the error propagates (app_main logs subsystem=wifi state=failed
         * phase=9 and keeps running degraded). */
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "init: esp_wifi_start failed: %s",
                     esp_err_to_name(err));
            return err;
        }
        s_wifi_started = true;
        wifi_config_t wifi_cfg;
        memset(&wifi_cfg, 0, sizeof(wifi_cfg));
        strlcpy((char *)wifi_cfg.sta.ssid, s_ssid_cache,
                sizeof(wifi_cfg.sta.ssid));
        strlcpy((char *)wifi_cfg.sta.password, password,
                sizeof(wifi_cfg.sta.password));
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        err = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "init: esp_wifi_set_config failed: %s",
                     esp_err_to_name(err));
            return err;
        }
        /* F2(b): a failed initial connect is NOT an init failure (the AP
         * may be down at boot) - the backoff chain takes over instead of
         * dying silently. */
        err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "init: esp_wifi_connect failed: %s",
                     esp_err_to_name(err));
            arm_reconnect();
        } else {
            ESP_LOGI(TAG, "subsystem=wifi state=connecting phase=9");
        }
        s_boot_plan = MICHI_BOOT_PLAN_CONNECT;
    } else {
        s_boot_plan = MICHI_BOOT_PLAN_PROVISION;
        ESP_LOGI(TAG, "subsystem=wifi state=unprovisioned phase=9");
    }

    /* Never kept beyond init (the driver config holds the copy). */
    memset(password, 0, sizeof(password));

    michi_state_register_observer(MICHI_EVENT_STATE_CHANGED, state_observer);
    michi_state_register_observer(MICHI_EVENT_RECOVER, recover_observer);

    s_initialized = true;
    ESP_LOGI(TAG, "subsystem=wifi state=ok phase=9");
    return ESP_OK;
}

esp_err_t michi_wifi_start_provisioning(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (michi_wifi_is_provisioned()) {
        ESP_LOGW(TAG, "provisioning: already provisioned (ssid=%s)",
                 michi_wifi_get_ssid());
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_mux);
    if (s_prov_active) {
        portEXIT_CRITICAL(&s_mux);
        return ESP_OK; /* idempotent */
    }
    s_prov_active = true;
    /* F3: a new session clears the abort flag - only shutdown/erase
     * (stop_provisioning_join) may set it again. */
    s_shutdown_requested = false;
    portEXIT_CRITICAL(&s_mux);

    /* Place the FSM (valid from IDLE and WIFI_CONNECTING; no-op when
     * already UNPROVISIONED). */
    const michi_state_t st = michi_state_get();
    if (st == MICHI_STATE_IDLE || st == MICHI_STATE_WIFI_CONNECTING) {
        michi_state_request(MICHI_STATE_UNPROVISIONED);
    }

    const BaseType_t rc = xTaskCreate(prov_task, "michi_wifi_prov",
                                      CONFIG_MICHI_WIFI_TASK_STACK_BYTES,
                                      NULL, MICHI_WIFI_PROV_TASK_PRIORITY,
                                      &s_prov_task);
    if (rc != pdPASS) {
        portENTER_CRITICAL(&s_mux);
        s_prov_active = false;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGE(TAG, "provisioning: task creation failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t michi_wifi_erase_credentials(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* F4: stop an active provisioning session and JOIN the task BEFORE
     * wiping the NVS namespace and the driver-persisted copy - the erase
     * must never race the persist. */
    portENTER_CRITICAL(&s_mux);
    const bool prov_active = s_prov_active;
    portEXIT_CRITICAL(&s_mux);
    if (prov_active) {
        stop_provisioning_join();
    }

    const esp_err_t err = wifi_nvs_erase_all();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase: nvs wipe failed: %s", esp_err_to_name(err));
        return err;
    }

    portENTER_CRITICAL(&s_mux);
    s_has_creds = false;
    s_creds_received = false;
    s_network_ready = false;
    s_ssid_cache[0] = '\0';
    portEXIT_CRITICAL(&s_mux);

    if (s_reconnect_timer != NULL) {
        esp_timer_stop(s_reconnect_timer);
    }
    if (s_prov_retry_timer != NULL) {
        esp_timer_stop(s_prov_retry_timer);
    }
    /* Discovery: stop announcing while the network profile is wiped. */
    (void)michi_discovery_stop();

    if (s_wifi_started) {
        /* Fires STA_DISCONNECTED: the handler sees no credentials and
         * does not schedule a reconnect. */
        esp_wifi_disconnect();
    }
    /* Wipe the Wi-Fi driver's persistent copy (wifi_prov_mgr stores the
     * delivered credentials with WIFI_STORAGE_FLASH): without this,
     * wifi_prov_mgr_is_provisioned() would still see the old config and
     * refuse a new provisioning. F13: the result is logged. */
    const esp_err_t restore_err = esp_wifi_restore();
    if (restore_err != ESP_OK) {
        ESP_LOGW(TAG, "erase: esp_wifi_restore failed: %s",
                 esp_err_to_name(restore_err));
    }

    const michi_state_t st = michi_state_get();
    if (st == MICHI_STATE_IDLE) {
        michi_state_request(MICHI_STATE_UNPROVISIONED);
    } else if (st == MICHI_STATE_WIFI_CONNECTING) {
        michi_state_post(MICHI_EVENT_WIFI_PROV_FAILED, 0);
    }

    ESP_LOGI(TAG, "subsystem=wifi state=unprovisioned phase=9");

    /* F7: restart the automatic provisioning cycle after the erase (the
     * device has no network until it is re-provisioned). */
    portENTER_CRITICAL(&s_mux);
    s_prov_retries = 0;
    portEXIT_CRITICAL(&s_mux);
    const esp_err_t perr = michi_wifi_start_provisioning();
    if (perr != ESP_OK) {
        ESP_LOGW(TAG, "erase: automatic provisioning restart failed: %s",
                 esp_err_to_name(perr));
    }
    return ESP_OK;
}

bool michi_wifi_is_provisioned(void)
{
    portENTER_CRITICAL(&s_mux);
    const bool v = s_has_creds;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

const char *michi_wifi_get_ssid(void)
{
    return s_ssid_cache;
}

esp_err_t michi_wifi_get_rssi(int8_t *out_rssi)
{
    if (out_rssi == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return ESP_ERR_NOT_FOUND; /* not connected: no link to measure */
    }
    *out_rssi = ap.rssi;
    return ESP_OK;
}

esp_err_t michi_wifi_get_reconnect_count(uint32_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_mux);
    *out = s_reconnects;
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t michi_wifi_shutdown(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    portENTER_CRITICAL(&s_mux);
    const bool prov_active = s_prov_active;
    s_initialized = false;
    portEXIT_CRITICAL(&s_mux);

    if (prov_active) {
        /* Cooperative provisioning stop (shared with erase, F4): the
         * manager is stopped, the abort flag set (F3) and the task
         * notified; the task deinitializes the manager and notifies the
         * joiner under the mux before deleting itself. */
        stop_provisioning_join();
    }

    if (s_reconnect_timer != NULL) {
        esp_timer_stop(s_reconnect_timer);
    }
    if (s_prov_retry_timer != NULL) {
        esp_timer_stop(s_prov_retry_timer);
    }

    if (s_wifi_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    /* Wipe the driver-persisted credentials BEFORE deinit (esp_wifi_restore
     * needs the wifi stack initialized). F13: the result is logged. */
    const esp_err_t restore_err = esp_wifi_restore();
    if (restore_err != ESP_OK) {
        ESP_LOGW(TAG, "shutdown: esp_wifi_restore failed: %s",
                 esp_err_to_name(restore_err));
    }
    esp_wifi_deinit();

    if (s_reconnect_timer != NULL) {
        esp_timer_stop(s_reconnect_timer);
        esp_timer_delete(s_reconnect_timer);
        s_reconnect_timer = NULL;
    }
    if (s_prov_retry_timer != NULL) {
        esp_timer_stop(s_prov_retry_timer);
        esp_timer_delete(s_prov_retry_timer);
        s_prov_retry_timer = NULL;
    }
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler);
    esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, event_handler);

    /* Wall clock (P0-02): join the sync task FIRST - after it exits no
     * michi_time sync callback (discovery announce resume) can race the
     * discovery teardown below - then deinit SNTP. */
    (void)michi_time_shutdown();

    /* Signed discovery: retire mDNS, close the UDP socket, stop the
     * timer and free the mDNS stack. */
    (void)michi_discovery_shutdown();

    ESP_LOGI(TAG, "subsystem=wifi state=off phase=9");
    return ESP_OK;
}
