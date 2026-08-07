#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"

#include "michi_board.h"
#include "michi_sd.h"

#define TAG "michi_sd"

/* Mount point for the card (the local OTA path in michi_ota reads
 * /sdcard/<name> through the VFS - the two components share the path
 * string, not a C dependency). */
#define MICHI_SD_MOUNT_POINT "/sdcard"

/* Open file handles for the FAT VFS: manifest probe + binary stream
 * (the OTA task) and nothing else - matches the mount max_files. */
#define MICHI_SD_MAX_FILES 5

/* Async mount (review F3): the mount task runs below the FSM (5) and the
 * display render (4) so it never delays the event bus or the screens; it
 * is above app_main (1). */
#define MICHI_SD_MOUNT_TASK_NAME "michi_sd_mnt"
#define MICHI_SD_MOUNT_TASK_PRIO 2
#define MICHI_SD_MOUNT_TASK_STACK_BYTES 4096
#define MICHI_SD_FLAG_POLL_MS 20
#define MICHI_SD_SHUTDOWN_WAIT_MS 10000

/* Lifecycle flags (F3): written by the mount task, read by every other
 * task through the mux. michi_sd_mounted() = s_mount_ok && s_mount_done:
 * while the mount is in progress the card is NOT reported as mounted. */
static portMUX_TYPE s_flags_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_mount_done;
static volatile bool s_mount_ok;
static sdmmc_card_t *s_card;
static TaskHandle_t s_mount_task;

/* Info cache (F4): esp_vfs_fat_info() is real SD I/O; the diagnostics
 * endpoint polls get_info on every request, so the TTL cache keeps the
 * card quiet between polls. The cache fields are guarded by s_info_mux
 * (the fill performs blocking I/O - a mutex, not a spinlock). */
static SemaphoreHandle_t s_info_mux;
static uint64_t s_cache_total;
static uint64_t s_cache_free;
static uint32_t s_cache_tick_ms;
static bool s_cache_valid;

static bool mount_flags_done(void)
{
    portENTER_CRITICAL(&s_flags_mux);
    const bool done = s_mount_done;
    portEXIT_CRITICAL(&s_flags_mux);
    return done;
}

static bool mount_flags_ok(void)
{
    portENTER_CRITICAL(&s_flags_mux);
    const bool ok = s_mount_done && s_mount_ok;
    portEXIT_CRITICAL(&s_flags_mux);
    return ok;
}

bool michi_sd_mounted(void)
{
    return mount_flags_ok();
}

esp_err_t michi_sd_get_info(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (total_bytes == NULL || free_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *total_bytes = 0;
    *free_bytes = 0;
    if (!michi_sd_mounted()) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_info_mux == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_info_mux, portMAX_DELAY);
    /* ms since boot; unsigned arithmetic stays correct across the 32-bit
     * wrap (the TTL is far shorter than the ~49 day wrap period). */
    const uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (s_cache_valid &&
        now_ms - s_cache_tick_ms < (uint32_t)CONFIG_MICHI_SD_INFO_TTL_MS) {
        *total_bytes = s_cache_total;
        *free_bytes = s_cache_free;
        xSemaphoreGive(s_info_mux);
        return ESP_OK;
    }
    /* esp_vfs_fat_info is the IDF 5.3 public API for FAT volume sizes;
     * the VFS layer has no statvfs in this release (verified against the
     * installed fatfs/vfs sources), so this is the honest way to expose
     * total/free bytes. */
    const esp_err_t err = esp_vfs_fat_info(MICHI_SD_MOUNT_POINT,
                                           total_bytes, free_bytes);
    if (err == ESP_OK) {
        s_cache_total = *total_bytes;
        s_cache_free = *free_bytes;
        s_cache_tick_ms = now_ms;
        s_cache_valid = true;
    } else {
        ESP_LOGW(TAG, "sd: info_failed err=%s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_info_mux);
    return err;
}

/* The mount itself: the body of the old synchronous michi_sd_init, run
 * from the mount task (F3) so the boot proceeds in parallel. */
static void do_mount(void)
{
    const michi_board_info_t *bi = michi_board_get_info();

    /* The SPI bus (SPI2_HOST, SCLK 39 / MOSI 38 / MISO 40) is initialized
     * by michi_board_init - michi_sd only attaches one more device (the
     * card, CS 41) to the existing bus. esp_vfs_fat_sdspi_mount() calls
     * sdspi_host_init_device() internally (IDF 5.3, vfs_fat_sdmmc.c:322),
     * so the bus and the LCD device stay untouched. */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    /* SDSPI_HOST_DEFAULT() already selects SPI2_HOST on the ESP32-S3 and
     * SDMMC_FREQ_DEFAULT (20 MHz); keep the frequency explicit so the
     * shared-bus intent is visible in one place. */
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    /* Review F5 (portability): do NOT name sdspi_device_config_t fields
     * that only exist in IDF 5.3.5+ (wait_for_miso was added in 5.3.5
     * and is absent in 5.3.0-5.3.4 and in 5.4/5.5 - naming it fails the
     * build on those releases). SDSPI_DEVICE_CONFIG_DEFAULT() exists in
     * every 5.3.x and gives the documented defaults (CD/WP/INT = NC,
     * wp_polarity = active-low); only the stable fields are overridden.
     *
     * Shared-bus safety: the ST7789 is write-only from the host's point
     * of view (it never drives MISO), and the SPI driver serializes
     * transactions per device - with distinct CS lines (LCD 45, SD 41)
     * the two devices never talk at the same time. On IDF 5.3.5+ the
     * default wait_for_miso (0 = 40 ms MISO-high wait before commands)
     * applies automatically, which is the safe choice for a multi-device
     * bus; on earlier 5.3.x the field does not exist and there is no
     * extra wait - behavior differs by IDF release, which is why the
     * field is deliberately never named here. */
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI2_HOST;
    slot.gpio_cs = (gpio_num_t)bi->sd_cs;
    slot.gpio_int = SDSPI_SLOT_NO_INT;

    esp_vfs_fat_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = MICHI_SD_MAX_FILES,
        .allocation_unit_size = 0,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    sdmmc_card_t *card = NULL;
    const esp_err_t err = esp_vfs_fat_sdspi_mount(MICHI_SD_MOUNT_POINT, &host,
                                                  &slot, &mount, &card);
    portENTER_CRITICAL(&s_flags_mux);
    if (err == ESP_OK) {
        s_card = card;
    }
    s_mount_done = true;
    s_mount_ok = (err == ESP_OK);
    portEXIT_CRITICAL(&s_flags_mux);

    if (err != ESP_OK) {
        /* Honest degradation: no card (or a card that cannot be mounted)
         * is a normal operating condition, not a fault. */
        ESP_LOGW(TAG, "sd: not mounted (ok - updates fall back to HTTPS OTA) "
                      "err=%s", esp_err_to_name(err));
        return;
    }

    uint64_t total_bytes = 0, free_bytes = 0;
    (void)michi_sd_get_info(&total_bytes, &free_bytes);
    ESP_LOGI(TAG, "sd: mounted=1 total=%llu free=%llu",
             (unsigned long long)total_bytes, (unsigned long long)free_bytes);
    ESP_LOGI(TAG, "sd: card name=%.8s max_freq_khz=%u real_freq_khz=%d",
             s_card->cid.name, (unsigned)s_card->max_freq_khz,
             s_card->real_freq_khz);
}

static void mount_task(void *arg)
{
    (void)arg;
    if (s_info_mux != NULL) {
        xSemaphoreTake(s_info_mux, portMAX_DELAY);
        s_cache_valid = false;
        xSemaphoreGive(s_info_mux);
    }
    do_mount();
    vTaskDelete(NULL);
}

esp_err_t michi_sd_init(void)
{
    if (s_info_mux == NULL) {
        s_info_mux = xSemaphoreCreateMutex();
        if (s_info_mux == NULL) {
            ESP_LOGE(TAG, "sd: info mutex creation failed");
            return ESP_ERR_NO_MEM;
        }
    }
    portENTER_CRITICAL(&s_flags_mux);
    const bool already = (s_mount_task != NULL || s_mount_done);
    portEXIT_CRITICAL(&s_flags_mux);
    if (already) {
        return ESP_OK; /* idempotent: a mount attempt is running or done */
    }
    BaseType_t rc = xTaskCreate(mount_task, MICHI_SD_MOUNT_TASK_NAME,
                                MICHI_SD_MOUNT_TASK_STACK_BYTES, NULL,
                                MICHI_SD_MOUNT_TASK_PRIO, &s_mount_task);
    if (rc != pdPASS) {
        portENTER_CRITICAL(&s_flags_mux);
        s_mount_task = NULL;
        portEXIT_CRITICAL(&s_flags_mux);
        ESP_LOGE(TAG, "sd: mount task creation failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t michi_sd_shutdown(void)
{
    /* F3: join an in-flight mount task before unmounting (the mount may
     * still hold the card / the VFS). Bounded: a hung mount must not
     * hang the shutdown. */
    const TickType_t deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(MICHI_SD_SHUTDOWN_WAIT_MS);
    while (!mount_flags_done() && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(MICHI_SD_FLAG_POLL_MS));
    }
    if (!mount_flags_done()) {
        ESP_LOGW(TAG, "sd: shutdown timeout waiting for mount task");
        return ESP_ERR_TIMEOUT;
    }
    if (!mount_flags_ok()) {
        return ESP_OK; /* never mounted */
    }
    esp_err_t err = esp_vfs_fat_sdcard_unmount(MICHI_SD_MOUNT_POINT, s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sd: unmount_failed err=%s", esp_err_to_name(err));
        return err;
    }
    portENTER_CRITICAL(&s_flags_mux);
    s_card = NULL;
    s_mount_done = false;
    s_mount_ok = false;
    portEXIT_CRITICAL(&s_flags_mux);
    if (s_info_mux != NULL) {
        xSemaphoreTake(s_info_mux, portMAX_DELAY);
        s_cache_valid = false;
        xSemaphoreGive(s_info_mux);
    }
    ESP_LOGI(TAG, "sd: unmounted");
    return ESP_OK;
}
