#pragma once
/* Shim for host-side tests: the Kconfig values the host-compiled
 * components build against. Mirrors the firmware Kconfig defaults:
 *  - firmware/components/michi_pairing/Kconfig (exact defaults);
 *  - firmware/components/michi_time/Kconfig (fast host values so the
 *    bounded SNTP wait tests run in ms, not seconds - the firmware
 *    defaults stay 10000 ms / 2 retries);
 *  - CONFIG_LWIP_SNTP_MAX_SERVERS mirrors the IDF 5.3 lwIP Kconfig
 *    default (the esp_netif_sntp shim struct needs it).
 * TEST-ONLY: never compiled into firmware. */

#define CONFIG_MICHI_PAIRING_WINDOW_SECONDS 120
#define CONFIG_MICHI_PAIRING_MAX_CONTROLLERS 8

#define CONFIG_MICHI_TIME_SNTP_SERVER "pool.ntp.org"
#define CONFIG_MICHI_TIME_SYNC_TIMEOUT_MS 250
#define CONFIG_MICHI_TIME_SYNC_RETRIES 3
#define CONFIG_MICHI_TIME_TASK_STACK_BYTES 3072

#define CONFIG_LWIP_SNTP_MAX_SERVERS 1
