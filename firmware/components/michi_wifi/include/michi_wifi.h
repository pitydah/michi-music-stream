#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Wi-Fi STA + BLE provisioning (phase 9).
 *
 * Owns the network bring-up of the universal firmware: STA interface,
 * connection with exponential backoff, BLE provisioning via
 * wifi_prov_mgr (NimBLE transport, WIFI_PROV_SECURITY_1/SRP6a with a
 * Kconfig proof-of-possession), mDNS announcements and the NVS
 * credentials store.
 *
 * Hard rules (see firmware/README.md, WiFi & provisioning section):
 * - Credentials live ONLY in the NVS "wifi" namespace (keys: ssid,
 *   password, device_name, region). Nothing is compiled in - Kconfig has
 *   no Wi-Fi credentials (only the provisioning PoP, MICHI_PROV_POP,
 *   which the protocol needs before the device is on any network). The
 *   password is NEVER logged, not partially: every log carries only the
 *   SSID.
 * - esp_event handlers are NON-BLOCKING (P0-11): they post FSM events
 *   and set flags; the reconnect with exponential backoff runs on a
 *   one-shot esp_timer, and the (blocking) BLE provisioning bring-up runs
 *   in the provisioning task.
 * - The FSM is the single state owner: michi_wifi only posts the phase-9
 *   events (WIFI_CONNECTED broadcast, WIFI_DISCONNECTED IDLE->
 *   WIFI_CONNECTING, WIFI_PROVISIONED UNPROVISIONED->WIFI_CONNECTING,
 *   NETWORK_READY WIFI_CONNECTING->IDLE, WIFI_PROV_FAILED
 *   WIFI_CONNECTING->UNPROVISIONED) or requests states; it never writes
 *   the state directly.
 *
 * Boot flow: michi_wifi_init() reads the "wifi" namespace - credentials
 * present -> STA connect (the FSM moves to WIFI_CONNECTING once it
 * reaches IDLE); absent -> the FSM lands on UNPROVISIONED and BLE
 * provisioning starts automatically. On reboot after provisioning the
 * stored credentials are used directly (no BLE).
 */

/**
 * @brief Initialize netif + Wi-Fi STA + event handlers + NVS "wifi"
 *        namespace + mDNS and auto-start (connect or provisioning).
 *
 * Runs the boot plan (connect vs provision) and defers the FSM placement
 * to a MICHI_EVENT_STATE_CHANGED observer: it acts when the FSM first
 * reaches IDLE (the boot events are posted later by app_main, so at
 * init() time the FSM is still BOOTING).
 *
 * Must be called after michi_state_init(), after init_nvs() and before
 * the boot events are posted. Safe to call once; repeated calls return
 * ESP_OK (idempotent). On failure app_main continues degraded: no
 * network, everything else keeps working.
 *
 * @return ESP_OK; ESP_FAIL when the STA netif cannot be created;
 *         esp_wifi/esp_timer/mdns errors are propagated unchanged.
 */
esp_err_t michi_wifi_init(void);

/**
 * @brief Start BLE provisioning (wifi_prov_mgr, NimBLE scheme).
 *
 * Idempotent: while a provisioning session is active, repeated calls
 * return ESP_OK. The blocking work (BLE controller + protocomm bring-up)
 * runs in the provisioning task, never in the caller.
 *
 * Credentials delivered by the client are persisted into the NVS "wifi"
 * namespace by the provisioning task; the custom endpoint
 * ("michi-device-info", JSON {"device_name":..., "region":...}) is
 * captured the same way and persisted with the credentials. On success
 * MICHI_EVENT_WIFI_PROVISIONED is posted (drives UNPROVISIONED ->
 * WIFI_CONNECTING when the FSM is in UNPROVISIONED).
 *
 * @return ESP_OK (session active or started); ESP_ERR_INVALID_STATE when
 *         already provisioned (erase first); ESP_ERR_NO_MEM when the
 *         provisioning task cannot be created.
 */
esp_err_t michi_wifi_start_provisioning(void);

/**
 * @brief Securely erase the credentials.
 *
 * Stops and joins an active provisioning session FIRST (the erase never
 * races the persist), erases the NVS "wifi" namespace, disconnects, wipes
 * the Wi-Fi driver's persistent copy (esp_wifi_restore - otherwise
 * wifi_prov_mgr would still see the old config and refuse a new
 * provisioning), returns the FSM to UNPROVISIONED and restarts the
 * automatic provisioning cycle.
 *
 * Relationship with the factory reset (phase 8 button): the factory
 * reset erases ALL of NVS (nvs_flash_erase + restart), this API erases
 * only the network profile while the device keeps running.
 *
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init; NVS errors are
 *         propagated unchanged.
 */
esp_err_t michi_wifi_erase_credentials(void);

/**
 * @brief Whether the NVS "wifi" namespace holds credentials (cached at
 *        init / provisioning).
 *
 * @return true when provisioned.
 */
bool michi_wifi_is_provisioned(void);

/**
 * @brief The stored SSID (cached; updated at init and after
 *        provisioning).
 *
 * @note The password is NEVER exposed nor logged (hard rule). The
 *       returned pointer is stable until the cache is updated; use it
 *       only for logs/UI, copy immediately if it must outlive that.
 *
 * @return The SSID, "" when not provisioned; never NULL.
 */
const char *michi_wifi_get_ssid(void);

/**
 * @brief Shut the network subsystem down: provisioning stopped (with
 *        cooperative task join), reconnect timer cancelled, handlers
 *        unregistered, Wi-Fi stopped/deinitialized, mDNS retired.
 *
 * Idempotent: a second call when the subsystem is already off returns
 * ESP_OK. On provisioning-join timeout the provisioning task is left to
 * wind down (warn logged).
 *
 * @return ESP_OK.
 */
esp_err_t michi_wifi_shutdown(void);

#ifdef __cplusplus
}
#endif
