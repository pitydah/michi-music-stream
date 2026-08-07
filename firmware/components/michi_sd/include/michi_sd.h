#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Onboard microSD support (phase 17): the Waveshare ESP32-S3-LCD-2
 *        card sits on the SAME SPI bus as the LCD (SCLK 39 / MOSI 38 /
 *        MISO 40, SPI2_HOST) with CS 41 - it is an ADDITIONAL device on
 *        the bus already initialized by michi_board, not a second bus.
 *
 * The card is mounted at /sdcard as a FAT filesystem and used for LOCAL
 * OTA updates (see michi_ota_start_local/check_local): the owner copies
 * a signed manifest + firmware binary onto the card, inserts it, and the
 * receiver applies the update at boot or on demand. The ESP32-S3 does
 * NOT boot from SD - this is update transport only, never a boot source.
 *
 * The mount is ASYNCHRONOUS (review F3): michi_sd_init() spawns a mount
 * task (priority 2) and returns immediately, so a boot without a card is
 * not penalized - the mount runs in parallel with the DAC/WiFi bring-up.
 * The mount outcome is published through michi_sd_mounted() and the
 * logs; callers that need the card (local OTA check) wait on the flag
 * with a bounded timeout (MICHI_SD_MOUNT_WAIT_MS).
 *
 * Bus sharing: the SPI driver serializes transactions per device, so an
 * SD transfer briefly pauses LCD flushing (order of ms for the 4 KB
 * update chunks) - no long locks, no deadlocks: michi_sd never performs
 * blocking multi-chunk transactions and the OTA path reads in 4 KB
 * chunks.
 *
 * Degradation contract: no card, card not formatted as FAT, or mount
 * failure are NOT fatal - the mount task logs and the system continues
 * with HTTPS OTA as the update path. All functions are safe to call when
 * the SD was never mounted.
 */

/**
 * @brief Start the asynchronous mount of the onboard microSD.
 *
 * Spawns the mount task (priority 2, below the FSM and the display task)
 * and returns immediately - the boot is never blocked by the card probe.
 * Mounts /sdcard (FAT, format_if_mount_failed=false - a card that needs
 * formatting is reported as absent, never formatted behind the owner's
 * back). The device is attached to SPI2_HOST (the bus initialized by
 * michi_board_init) with CS 41; esp_vfs_fat_sdspi_mount() attaches the
 * SD device to the existing bus via sdspi_host_init_device internally
 * (verified in the IDF 5.3 sources, vfs_fat_sdmmc.c) - michi_sd does not
 * initialize the bus nor call sdspi_host_init_device itself, so the LCD
 * device stays untouched.
 *
 * MUST run after michi_board_init() (the SPI bus owner); idempotent -
 * repeated calls return ESP_OK while a mount attempt is running or done
 * (a failed mount is NOT retried by a repeated call in the same boot).
 * Poll michi_sd_mounted() (or wait up to MICHI_SD_MOUNT_WAIT_MS) for
 * the outcome; the mount task logs the result either way.
 *
 * @return ESP_OK (mount task started, or an attempt already ran);
 *         ESP_ERR_NO_MEM on task/mutex allocation failure.
 */
esp_err_t michi_sd_init(void);

/**
 * @brief Whether the card is mounted and /sdcard is usable.
 *
 * @return true when the mount task completed successfully (mount flags
 *         s_mount_ok && s_mount_done, published under the flags mux);
 *         false while the mount is in progress, on failure, and before
 *         michi_sd_init().
 */
bool michi_sd_mounted(void);

/**
 * @brief Volume sizes of the mounted card (FAT filesystem statistics).
 *
 * Uses esp_vfs_fat_info() (the IDF 5.3 public API for FAT volume total/
 * free bytes - there is no statvfs in the IDF 5.3 VFS layer, verified
 * against the installed sources). Values are filesystem statistics, not
 * raw card capacity: formatting overhead is excluded.
 *
 * The result is CACHED for CONFIG_MICHI_SD_INFO_TTL_MS (default 30 s):
 * the first call performs the real SD I/O, subsequent calls within the
 * TTL return the cached copy (the diagnostics endpoint polls this on
 * every /diagnostics request - the cache keeps the card quiet). The
 * cache is invalidated on mount and unmount. Thread-safe (mutex).
 *
 * @param total_bytes Out: total filesystem bytes (0 when not mounted).
 * @param free_bytes  Out: free filesystem bytes (0 when not mounted).
 * @return ESP_OK; ESP_ERR_INVALID_ARG on NULL out; ESP_ERR_NOT_FOUND when
 *         the card is not mounted (out set to 0).
 */
esp_err_t michi_sd_get_info(uint64_t *total_bytes, uint64_t *free_bytes);

/**
 * @brief Unmount the card and release the VFS path. Safe when never
 *        mounted (no-op). Waits for an in-flight mount task (bounded)
 *        before unmounting. The card can be re-mounted with
 *        michi_sd_init().
 *
 * @return ESP_OK; ESP_ERR_TIMEOUT when the mount task did not finish in
 *         time (nothing was unmounted).
 */
esp_err_t michi_sd_shutdown(void);

#ifdef __cplusplus
}
#endif
