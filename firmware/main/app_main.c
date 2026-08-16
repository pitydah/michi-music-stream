#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"

#include "michi_audio.h"
#include "michi_board.h"
#include "michi_button.h"
#include "michi_dac.h"
#include "michi_display.h"
#include "michi_http.h"
#include "michi_led.h"
#include "michi_log.h"
#include "michi_ota.h"
#include "michi_pairing.h"
#include "michi_product_profile.h"
#include "michi_sd.h"
#include "michi_session.h"
#include "michi_state.h"
#include "michi_version.h"
#include "michi_wifi.h"

static const char *TAG = "michi_app";

/* MS-06: the pairing component hands the freshly generated PIN to the
 * LOCAL screen through this callback (the PIN is never returned by
 * HTTP). NULL clears the screen (window closed/expired, confirm,
 * reboot). */
static void pairing_pin_display_cb(const char *pin, void *ctx)
{
    (void)ctx;
    if (pin != NULL) {
        (void)michi_display_show_pairing_pin(pin);
    } else {
        (void)michi_display_clear_pairing_pin();
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs_flash_init returned %s, erasing NVS and retrying once",
                 esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase failed: %s", esp_err_to_name(err));
            return err;
        }
        err = nvs_flash_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_init retry failed: %s", esp_err_to_name(err));
            return err;
        }
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "nvs_flash initialized");
    return ESP_OK;
}

static void log_selftest_rows(const michi_board_info_t *info,
                              const michi_board_selftest_t *st)
{
    ESP_LOGI(TAG, "board=%s chip=%s",
             info->model, st->chip_ok ? "ok" : "FAIL");
    ESP_LOGI(TAG, "flash=%" PRIu32 " bytes (expected %" PRIu32 ") status=%s",
             st->flash_bytes, info->flash_bytes_expected,
             st->flash_ok ? "ok" : "FAIL");
    ESP_LOGI(TAG, "psram=%" PRIu32 " bytes (expected %" PRIu32 ") status=%s",
             st->psram_bytes, info->psram_bytes_expected,
             st->psram_ok ? "ok" : "FAIL");
    ESP_LOGI(TAG, "display=%s", st->display_ok ? "ok" : "FAIL");
    ESP_LOGI(TAG, "backlight=%s", st->backlight_ok ? "ok" : "FAIL");
    ESP_LOGI(TAG, "wifi_supported=%s", st->wifi_supported ? "yes" : "no");
    ESP_LOGI(TAG, "ble_supported=%s", st->ble_supported ? "yes" : "no");
    ESP_LOGI(TAG, "dac_model=%s dac_ok=%s",
             st->dac_model[0] != '\0' ? st->dac_model : "none",
             st->dac_ok ? "true" : "false");
    ESP_LOGI(TAG, "selftest=%s", st->overall ? "PASS" : "DEGRADED");
}

static void log_pending_subsystems(void)
{
    ESP_LOGI(TAG, "subsystem=bsp state=ok phase=1");
    ESP_LOGI(TAG, "subsystem=api state=ok phase=12");
}

/* DAC phase 2 bring-up: init -> detect -> start. Honest at every step: no
 * DAC, no clocks, init failure are all reported instead of faked. */
static void init_dac(void)
{
    esp_err_t err = michi_dac_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "michi_dac_init failed: %s (audio unavailable)",
                 esp_err_to_name(err));
        ESP_LOGI(TAG, "subsystem=dac state=failed phase=2");
        return;
    }
    err = michi_dac_detect();
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "no DAC detected (probe 0x4D..0x4F + sanity): audio unavailable");
        ESP_LOGI(TAG, "subsystem=dac state=absent phase=2");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "michi_dac_detect failed: %s (audio unavailable)",
                 esp_err_to_name(err));
        ESP_LOGI(TAG, "subsystem=dac state=error phase=2");
        return;
    }

    ESP_LOGI(TAG, "subsystem=dac state=detected phase=2");

    err = michi_dac_start(48000, 16, 2);
    if (err != ESP_OK) {
        /* Honest diagnostic: the DAC answered I2C but cannot be initialized.
         * For the PCM5122 this is expected while no I2S master is running
         * (PLL cannot lock without BCLK/LRCK); phase 11 starts the clocks. */
        ESP_LOGE(TAG, "michi_dac_start(48000,16,2) failed: %s - "
                 "dac detected but NOT initialized (audio_available=false)",
                 esp_err_to_name(err));
        ESP_LOGI(TAG, "subsystem=dac state=detected_init_failed phase=2");
        return;
    }
    ESP_LOGI(TAG, "dac=ok");
    ESP_LOGI(TAG, "subsystem=dac state=initialized phase=2");
}

void app_main(void)
{
#ifdef CONFIG_MICHI_DAC_MOCK
    ESP_LOGW(TAG, "MICHI_DAC_MOCK is ENABLED - this build fakes a DAC and "
             "must NOT be used in production");
#endif
    ESP_LOGI(TAG, "michi-music-stream firmware v%s target=%s",
             MICHI_FW_VERSION_STR, CONFIG_IDF_TARGET);

    esp_err_t err;

    /* Log registry (phase 16): hybrid tail + journal. Init FIRST (before
     * NVS, before any other PSRAM user): the tail ring is the first
     * PSRAM allocation of the boot, so its address is deterministic and
     * the crash dump can validate the previous boot's ring. The FSM
     * observer for the journal is registered here (allowed pre-init). */
    err = michi_log_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "michi_log_init failed: %s (no log registry)",
                 esp_err_to_name(err));
    }

    /* State machine (phase 5): the single global coordinator. Init FIRST,
     * before NVS, so the NVS-fatal path can land on the real terminal state
     * (BOOTING->FATAL_ERROR is in the table) and every later producer can
     * post events. */
    err = michi_state_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "state bus unavailable - all events will be dropped");
        ESP_LOGI(TAG, "subsystem=state state=failed phase=5");
    }
    const bool state_ok = (err == ESP_OK);

    /* Display subsystem (phase 6): dynamic state screens rendered by the
     * display task. BOOTING/SELF_TEST stay covered by the BSP boot screen
     * (rendered below, before the boot events); on failure boot continues
     * degraded - no dynamic screens, the boot screen still shows. */
    err = michi_display_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "michi_display_init failed: %s (no dynamic screens)",
                 esp_err_to_name(err));
        ESP_LOGI(TAG, "subsystem=display state=failed phase=6");
    }

    /* LED subsystem (phase 7): SK6812 status LEDs (M5Stack U003) driven by
     * the animation task. The observer never touches the strip (MUST-NOT-
     * block contract); on failure boot continues degraded - no status
     * LEDs, everything else keeps working. */
    err = michi_led_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "michi_led_init failed: %s (no status LEDs)",
                 esp_err_to_name(err));
        ESP_LOGI(TAG, "subsystem=led state=failed phase=7");
    }

    /* Pairing button (phase 8): debounced physical button that posts the
     * pairing/recovery events and executes the physical factory reset
     * (identity + pairing wipe + full NVS erase + restart, only from
     * stable states); the ISR records edges and timestamps, the debounce
     * task runs the actions. On failure boot continues degraded - no
     * button, everything else keeps working. */
    err = michi_button_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "michi_button_init failed: %s (no pairing button)",
                 esp_err_to_name(err));
        ESP_LOGI(TAG, "subsystem=button state=failed phase=8");
    }

    err = init_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "FATAL: NVS is unusable (%s), halting - subsystems depending on NVS cannot start",
                 esp_err_to_name(err));
        /* The FSM was initialized before NVS, so this request is real and
         * lands on the terminal state. */
        michi_state_request(MICHI_STATE_FATAL_ERROR);
        for (;;) {
            /* This loop is NOT subscribed to the task watchdog:
             * esp_task_wdt_reset() only feeds tasks registered via
             * esp_task_wdt_add() (or the startup defaults), and app_main is
             * neither - the call is a no-op. It is kept so the halt intent
             * is explicit and the pattern survives if the watchdog
             * subscription changes; a real WDT reset would not help anyway:
             * the device is intentionally halted after a terminal error. */
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    /* Log journal (phase 16): SPIFFS mount + boot_seq + journal task,
     * right after NVS (boot_seq lives there; the crash dump flush needs
     * SPIFFS). On failure boot continues degraded - tail keeps working. */
    err = michi_log_start_journal();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "michi_log_start_journal failed: %s "
                      "(journal disabled, tail still active)",
                 esp_err_to_name(err));
        ESP_LOGI(TAG, "subsystem=log state=degraded phase=16");
    } else {
        ESP_LOGI(TAG, "subsystem=log state=ok phase=16");
    }

    /* Network subsystem (phase 9): Wi-Fi STA + BLE provisioning +
     * mDNS. Runs AFTER init_nvs() (the credentials live in the NVS
     * "wifi" namespace) and BEFORE the boot events: the FSM is still
     * BOOTING, and michi_wifi places itself (WIFI_CONNECTING or
     * UNPROVISIONED) through its STATE_CHANGED observer once the FSM
     * reaches IDLE. On failure boot continues degraded - no network,
     * everything else keeps working. */
    err = michi_wifi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "michi_wifi_init failed: %s (no network)",
                 esp_err_to_name(err));
        ESP_LOGI(TAG, "subsystem=wifi state=failed phase=9");
    }

    /* Pairing & security (phase 10): controller registry (tokens stored
     * as SHA-256 digests only) + the button-authorized pairing window.
     * The window opens ONLY from the physical button (michi_button); the
     * HTTP handlers operate strictly inside it. On failure boot
     * continues degraded - no pairing, everything else keeps working. */
    err = michi_pairing_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "michi_pairing_init failed: %s (no pairing)",
                 esp_err_to_name(err));
        ESP_LOGI(TAG, "subsystem=pairing state=failed phase=10");
    } else {
        /* MS-06: the PIN generated by pair/start is shown on the LOCAL
         * panel through this callback - it is never returned by HTTP.
         * Registered after michi_display_init (the display subsystem is
         * up; the callback degrades gracefully if it were not). */
        michi_pairing_set_pin_display_cb(pairing_pin_display_cb, NULL);
    }

    err = michi_board_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "michi_board_init failed (%s), continuing in degraded mode",
                 esp_err_to_name(err));
    }

    const michi_board_info_t *info = michi_board_get_info();
    michi_board_selftest_t st = michi_board_self_test();

    /* microSD (phase 17): onboard card on the LCD SPI bus (CS 41). MUST
     * run AFTER michi_board_init (SPI2_HOST is initialized by the BSP):
     * the card is one more device on that bus, not a second bus. The
     * mount is ASYNC (review F3): michi_sd_init spawns the mount task
     * and returns immediately - the boot is not penalized when no card
     * is present (the mount runs in parallel with the DAC/WiFi bring-up);
     * the outcome is published via michi_sd_mounted() + the mount task
     * logs. Degraded when absent - local OTA unavailable, HTTPS OTA
     * keeps working. */
#ifdef CONFIG_MICHI_SD_ENABLE
    err = michi_sd_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "michi_sd_init failed: %s (no local updates)",
                 esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "subsystem=sd state=starting phase=17");
#endif

    init_dac();

    /* Audio engine (phase 11): RTP/UDP session receiver + jitter buffer.
     * michi_audio_init() validates the phase-11 constants; the session
     * engine does NOT start at boot (sessions arrive with phase 12).
     * michi_audio_boot_dac() starts the I2S clocks (silence) so the
     * PCM5122 PLL can lock, then re-runs michi_dac_start() for the
     * validated profile: detected-but-uninitialized becomes initialized;
     * with no DAC the profile stays DIAGNOSTIC (honest). Runs BEFORE the
     * self-test rows and the profile build so the boot screen and the
     * profile log reflect the real audio state. */
    err = michi_audio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "michi_audio_init failed: %s", esp_err_to_name(err));
        ESP_LOGI(TAG, "subsystem=audio state=failed phase=11");
    } else {
        ESP_LOGI(TAG, "subsystem=audio state=ok phase=11");
        const michi_dac_caps_t *audio_caps = michi_dac_get_caps();
        if (audio_caps->detected && !audio_caps->initialized) {
            err = michi_audio_boot_dac();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "michi_audio_boot_dac failed: %s "
                              "(profile stays diagnostic)",
                         esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "subsystem=dac state=initialized phase=11");
            }
        } else if (!audio_caps->detected) {
            ESP_LOGW(TAG, "no DAC detected: I2S/DAC boot skipped "
                          "(profile stays diagnostic)");
        }
    }

    /* Session layer (phase 12): the single active session lifecycle
     * (start/stop/patch) over the RTP engine. Must run after
     * michi_audio_init() - it calls into the engine; the HTTP handlers
     * (michi_http_init below) call into it at request time. On failure
     * boot continues degraded - no sessions, the rest keeps working. */
    err = michi_session_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "michi_session_init failed: %s (no sessions)",
                 esp_err_to_name(err));
        ESP_LOGI(TAG, "subsystem=session state=failed phase=12");
    }

    /* OTA subsystem (phase 13): signed updates with A/B rollback. Runs
     * after the session layer (the update path force-closes the active
     * session) and logs the running partition + image state at boot.
     * On failure boot continues degraded - no OTA, the rest works. */
    err = michi_ota_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "michi_ota_init failed: %s (no OTA updates)",
                 esp_err_to_name(err));
        ESP_LOGI(TAG, "subsystem=ota state=failed phase=13");
    }

    const michi_dac_caps_t *caps = michi_dac_get_caps();
    snprintf(st.dac_model, sizeof(st.dac_model), "%s",
             caps->detected ? caps->model : "");
    st.dac_ok = caps->initialized;

    log_selftest_rows(info, &st);

    /* Product profile: the single source of truth derived from DAC caps and
     * board evidence. Everything that announces the product (logs, boot
     * screen, later API/mDNS/BLE/sessions) reads from this profile - no
     * duplicated strings. */
    err = michi_product_profile_init();
    if (err != ESP_OK) {
        /* Defensive only: init cannot fail with the current evidence sources. */
        ESP_LOGE(TAG, "michi_product_profile_init failed: %s", esp_err_to_name(err));
        ESP_LOGI(TAG, "subsystem=product_profile state=failed phase=3");
    } else {
        ESP_LOGI(TAG, "subsystem=product_profile state=ok phase=3");
    }
    const michi_product_profile_t *profile = michi_product_profile_get();
    char codecs_str[40] = {0};
    char rates_str[40] = {0};
    michi_product_profile_format_codecs(profile, codecs_str, sizeof(codecs_str));
    michi_product_profile_format_rates(profile, rates_str, sizeof(rates_str));
    ESP_LOGI(TAG, "profile: name=%s tier=%s audio_available=%s dac=%s "
             "codecs=%s sample_rates=%s display=%s lighting_rgb=%s "
             "cat_contour=%s",
             profile->product_name, michi_product_profile_tier_name(),
             profile->audio_available ? "true" : "false",
             profile->dac_model, codecs_str, rates_str,
             profile->display_present ? "true" : "false",
             profile->lighting_status_rgb ? "true" : "false",
             profile->lighting_cat_contour ? "true" : "false");

    /* OTA rollback self-test (phase 13): after the board self-test + the
     * profile build. Criterion (documented in michi_ota.h): the BOARD
     * self-test overall (chip/flash/psram/display/backlight); a
     * DIAGNOSTIC profile (no DAC detected) is a legitimate hardware
     * option and does NOT block the mark. On the first boot after an OTA
     * the image is PENDING_VERIFY: pass marks it valid (cancel rollback),
     * fail logs + restarts so the bootloader rolls back. Any other image
     * state is a no-op. */
    michi_ota_boot_selftest_done(st.overall);

    /* HTTP API (phase 4): read-only migrated endpoints (/info, /firmware).
     * A failure is logged and boot continues - no halt. */
    err = michi_http_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "michi_http_init failed: %s (API /info and /firmware unavailable)",
                 esp_err_to_name(err));
        ESP_LOGI(TAG, "subsystem=http state=failed phase=4");
    } else {
        ESP_LOGI(TAG, "subsystem=http state=ok phase=4");
    }

    /* Early redraw of the product boot screen: the render task (still in
     * state BOOTING, the panel is available after board_init) paints
     * "michi iniciando" via michi_ui_draw_screen_boot while the rest of
     * the boot continues; the boot events below then drive BOOTING ->
     * SELF_TEST -> IDLE through the same task. The BSP legacy boot screen
     * (michi_board_display_boot_screen) is intentionally NOT called in
     * the normal flow anymore - its technical content (Board:/Flash:/...
     * /Result:) lives in the logs and on the diagnostics screen; the
     * function stays in the BSP for future diagnostics. */
    if (st.display_ok) {
        michi_display_request_redraw();
    } else {
        ESP_LOGW(TAG, "display unavailable, boot screen skipped (degraded mode)");
    }

    /* Boot events, posted after all boot-critical inits (NVS, board, self
     * test, DAC, profile, HTTP, early boot-screen redraw): BOOT_COMPLETE
     * drives
     * BOOTING->SELF_TEST and SELF_TEST_DONE drives SELF_TEST->IDLE with ANY
     * data. The self-test already ran before these events are posted - the
     * SELF_TEST state is modeled retrospectively, so observers must not
     * expect to observe the test window; the overall result is surfaced by
     * the log below. RECOVERABLE_ERROR has no boot path: it is reserved for
     * runtime producers arriving from phase 9. */
    if (state_ok) {
        err = michi_state_post(MICHI_EVENT_BOOT_COMPLETE, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "MICHI_EVENT_BOOT_COMPLETE post failed: %s",
                     esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "self_test: overall=%u", (unsigned)(st.overall ? 1u : 0u));
        err = michi_state_post(MICHI_EVENT_SELF_TEST_DONE, st.overall ? 1u : 0u);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "MICHI_EVENT_SELF_TEST_DONE post failed: %s",
                     esp_err_to_name(err));
        }
    }

    /* Local (SD) update check (phase 17, review F2): NOT called directly
     * anymore. The FSM observer registered by michi_ota_init triggers
     * the check task when the FSM reaches IDLE - the MICHI_STATE_UPDATING
     * request maps only from IDLE, so a direct call here (the boot events
     * are only queued at this point, the FSM is still BOOTING/SELF_TEST)
     * would start the update without the UPDATING state: no LED ramp, no
     * "Updating" screen, diagnostics stuck on IDLE. The check task waits
     * for the async SD mount (MICHI_SD_MOUNT_WAIT_MS) and runs the F1
     * latches before starting. */
#ifdef CONFIG_MICHI_SD_ENABLE
    ESP_LOGI(TAG, "subsystem=ota_local state=armed phase=17");
#endif

    log_pending_subsystems();

    ESP_LOGI(TAG, "boot=ok mode=%s audio_available=%s",
             michi_product_profile_tier_name(),
             profile->audio_available ? "true" : "false");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
