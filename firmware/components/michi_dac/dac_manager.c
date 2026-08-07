/*
 * DAC manager: orchestration of the subsystem.
 *
 * Flow: michi_dac_init() (registry + NVS profile, I2C bus created lazily) ->
 *        michi_dac_detect() (probe or force-bind) ->
 *        michi_dac_start() (driver init + configure).
 * Lifecycle state: NONE -> DETECTED -> INITIALIZED. Errors are propagated,
 * nothing is faked. michi_dac_shutdown() unbinds the driver (back to NONE),
 * so the full cycle init -> detect -> start -> shutdown -> detect -> start
 * works.
 *
 * Detection sources, in order:
 *  1. NVS "dac_profile" (namespace "michi_dac", string <= 63 chars): a set
 *     value force-binds the driver with the matching board_profile
 *     ("pcm5122", "pcm5102a"). Detection of PCM5122 over I2C is stable, so a
 *     successful probe NEVER writes the profile back. An unknown profile is
 *     logged and falls back to autodetection.
 *  2. Registered hardware-ID source (extension point for boards with an
 *     EEPROM carrying the DAC identity; no such hardware exists yet).
 *  3. Autodetection: walk the registry and probe every self_detectable
 *     driver in order.
 *
 * The I2C bus is only created when something may actually talk over it: a
 * self-detectable driver in the registry, or a profile that binds an I2C DAC.
 * Profile-only non-I2C DACs (PCM5102A) never touch the bus.
 */

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "michi_dac.h"
#include "michi_dac_types.h"

#include "dac_internal.h"

#define MICHI_DAC_NVS_NAMESPACE "michi_dac"
#define MICHI_DAC_NVS_KEY_PROFILE "dac_profile"
#define MICHI_DAC_PROFILE_BUF_LEN 64 /* NVS string buffer, enough for any DAC profile */

#define MICHI_DAC_I2C_PORT I2C_NUM_0

static const char *TAG = "michi_dac";

static i2c_master_bus_handle_t s_bus = NULL;
static const michi_dac_driver_t *s_bound = NULL;
static michi_dac_state_t s_state = MICHI_DAC_STATE_NONE;
static bool s_board_verified = false;
static char s_bind_reason[32] = {0};
static michi_dac_hw_id_fn s_hw_id_source = NULL;
static michi_dac_caps_t s_caps = {0};
static bool s_inited = false;

/* Reads the NVS DAC profile. Robust by design: an oversized or corrupt value
 * never fails the subsystem - it is warned about, erased and treated as "no
 * profile" so autodetection can still run. */
static esp_err_t load_profile_from_nvs(char *profile, size_t buf_len)
{
    profile[0] = '\0';
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(MICHI_DAC_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* namespace never written: no profile */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    size_t len = 0;
    err = nvs_get_str(handle, MICHI_DAC_NVS_KEY_PROFILE, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_OK; /* key never written: no profile */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_str(length) failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }
    if (len >= buf_len) {
        /* Oversized profile: warn, erase, treat as absent. Never fail the
         * whole subsystem because of a bad NVS value. */
        ESP_LOGW(TAG, "dac_profile too long (%u bytes, max %u): erasing it, "
                 "falling back to autodetection",
                 (unsigned)len, (unsigned)buf_len - 1);
        nvs_close(handle);
        nvs_handle_t rw = 0;
        err = nvs_open(MICHI_DAC_NVS_NAMESPACE, NVS_READWRITE, &rw);
        if (err == ESP_OK) {
            nvs_erase_key(rw, MICHI_DAC_NVS_KEY_PROFILE);
            nvs_commit(rw);
            nvs_close(rw);
        } else {
            ESP_LOGW(TAG, "nvs_open(RW) for erase failed: %s (profile left as-is)",
                     esp_err_to_name(err));
        }
        return ESP_OK;
    }
    len = buf_len;
    err = nvs_get_str(handle, MICHI_DAC_NVS_KEY_PROFILE, profile, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    nvs_close(handle);
    return err;
}

/* I2C bus needed only when something may actually talk over it: a profile
 * binding an I2C DAC (self-detectable implies an I2C probe; PCM5102A is not
 * self-detectable and has no control bus), or autodetection candidates.
 * An unknown profile may fall back to autodetection, which needs the bus. */
static bool i2c_bus_needed(const char *profile)
{
    if (profile[0] != '\0') {
        const michi_dac_driver_t *drv = michi_dac_registry_find_by_profile(profile);
        if (drv != NULL && !drv->self_detectable) {
            return false; /* profile-only non-I2C DAC (e.g. pcm5102a) */
        }
        return true;
    }
    for (size_t i = 0; i < michi_dac_registry_count(); i++) {
        const michi_dac_driver_t *drv = michi_dac_registry_get(i);
        if (drv != NULL && drv->self_detectable) {
            return true;
        }
    }
    return false;
}

static esp_err_t bind_driver(const michi_dac_driver_t *drv, const char *reason)
{
    if (drv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_bound = drv;
    s_state = MICHI_DAC_STATE_DETECTED;
    /* "Verified" means the binding is backed by hardware evidence: a real
     * probe (ACK + reset-default sanity + round-trip). An NVS profile is a
     * user assertion, NOT verification: board_verified stays false and the
     * log says so explicitly (profile-asserted, unverified). */
    s_board_verified = (strcmp(reason, "probe") == 0);
    snprintf(s_bind_reason, sizeof(s_bind_reason), "%s", reason);
    if (strcmp(reason, "profile") == 0) {
        ESP_LOGI(TAG, "%s bound by profile (profile-asserted, unverified)",
                 drv->name);
    } else {
        ESP_LOGI(TAG, "bound driver=%s profile=%s reason=%s", drv->name,
                 drv->board_profile != NULL ? drv->board_profile : "?", reason);
    }
    return ESP_OK;
}

esp_err_t michi_dac_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    char profile[MICHI_DAC_PROFILE_BUF_LEN] = {0};
    esp_err_t err = load_profile_from_nvs(profile, sizeof(profile));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS profile read failed: %s", esp_err_to_name(err));
        return err;
    }
    if (profile[0] != '\0') {
        ESP_LOGI(TAG, "NVS dac_profile=%s (force-bind source)", profile);
    }

    if (i2c_bus_needed(profile)) {
        /* I2C master bus on Kconfig pins, 100 kHz by default. Internal
         * pull-ups enabled as a fallback; the README requires measuring
         * external pull-ups (2.2-4.7 kOhm) before raising the speed to
         * 400 kHz (internal ~45 kOhm are marginal there). */
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = MICHI_DAC_I2C_PORT,
            .sda_io_num = CONFIG_MICHI_DAC_I2C_SDA,
            .scl_io_num = CONFIG_MICHI_DAC_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .trans_queue_depth = 4,
            .flags.enable_internal_pullup = true,
        };
        err = i2c_new_master_bus(&bus_cfg, &s_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "I2C bus ok: port=%d sda=%d scl=%d speed=%d Hz",
                 MICHI_DAC_I2C_PORT, CONFIG_MICHI_DAC_I2C_SDA,
                 CONFIG_MICHI_DAC_I2C_SCL, CONFIG_MICHI_DAC_I2C_SPEED_HZ);
    } else {
        ESP_LOGI(TAG, "I2C bus skipped: no I2C DAC in use (profile-only non-I2C DAC)");
    }

    s_inited = true;
    ESP_LOGI(TAG, "init ok: registry=%d drivers, nvs_profile=%s",
             (int)michi_dac_registry_count(), profile[0] != '\0' ? "set" : "empty");
    return ESP_OK;
}

static esp_err_t detect_by_profile(const char *profile)
{
    const michi_dac_driver_t *drv = michi_dac_registry_find_by_profile(profile);
    if (drv == NULL) {
        ESP_LOGW(TAG, "NVS profile %s matches no registered driver", profile);
        return ESP_ERR_NOT_FOUND;
    }
    return bind_driver(drv, "profile");
}

static esp_err_t autodetect(void)
{
    for (size_t i = 0; i < michi_dac_registry_count(); i++) {
        const michi_dac_driver_t *drv = michi_dac_registry_get(i);
        if (drv == NULL || !drv->self_detectable) {
            continue;
        }
        ESP_LOGI(TAG, "probing driver=%s (self-detectable)", drv->name);
        esp_err_t err = drv->ops.probe(drv, (void *)s_bus);
        if (err == ESP_OK) {
            return bind_driver(drv, "probe");
        }
        if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGI(TAG, "driver=%s not found (%s)", drv->name, esp_err_to_name(err));
            continue;
        }
        ESP_LOGE(TAG, "driver=%s probe failed with bus error: %s",
                 drv->name, esp_err_to_name(err));
        return err; /* real bus problem: propagate, do not mask */
    }
    return ESP_ERR_NOT_FOUND; /* honest: no DAC found */
}

esp_err_t michi_dac_detect(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_bound != NULL) {
        return ESP_OK; /* idempotent */
    }

    char profile[MICHI_DAC_PROFILE_BUF_LEN] = {0};
    esp_err_t err = load_profile_from_nvs(profile, sizeof(profile));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS profile read failed: %s", esp_err_to_name(err));
        return err;
    }
    bool had_profile = (profile[0] != '\0');
    if (had_profile) {
        err = detect_by_profile(profile);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (err != ESP_ERR_NOT_FOUND) {
            return err;
        }
        ESP_LOGW(TAG, "unknown dac_profile='%s', falling back to autodetect", profile);
    }

    if (s_hw_id_source != NULL) {
        char hw_profile[MICHI_DAC_PROFILE_BUF_LEN] = {0};
        err = s_hw_id_source(hw_profile, sizeof(hw_profile));
        if (err == ESP_OK && hw_profile[0] != '\0') {
            ESP_LOGI(TAG, "hw-id source reported profile=%s", hw_profile);
            const michi_dac_driver_t *drv = michi_dac_registry_find_by_profile(hw_profile);
            if (drv == NULL) {
                ESP_LOGW(TAG, "hw-id profile %s matches no registered driver", hw_profile);
                return ESP_ERR_NOT_FOUND;
            }
            return bind_driver(drv, "hw_id");
        }
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "hw-id source failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    err = autodetect();
    if (err != ESP_ERR_NOT_FOUND) {
        return err;
    }
    /* Two distinct causes for the same outcome, logged differently: no
     * profile was ever set (autodetection is the only source) vs a profile
     * was present but its probe fallback found nothing. */
    if (had_profile) {
        ESP_LOGW(TAG, "no DAC detected: probe fallback after unknown profile found no device");
    } else {
        ESP_LOGW(TAG, "no DAC detected: no dac_profile set and probe found no device");
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t michi_dac_start(uint32_t sample_rate, uint8_t bit_depth, uint8_t channels)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state == MICHI_DAC_STATE_INITIALIZED) {
        return ESP_OK; /* idempotent */
    }
    if (s_state != MICHI_DAC_STATE_DETECTED || s_bound == NULL) {
        ESP_LOGE(TAG, "start requires DETECTED (state=%s)", s_state == MICHI_DAC_STATE_NONE ? "NONE" : "?");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = s_bound->ops.init(s_bound, (void *)s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "driver=%s init failed: %s (state stays DETECTED)",
                 s_bound->name, esp_err_to_name(err));
        return err;
    }
    err = s_bound->ops.configure(s_bound, (void *)s_bus, sample_rate, bit_depth, channels);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "driver=%s configure failed: %s (state stays DETECTED)",
                 s_bound->name, esp_err_to_name(err));
        return err;
    }
    s_state = MICHI_DAC_STATE_INITIALIZED;
    ESP_LOGI(TAG, "start ok: driver=%s %" PRIu32 " Hz / %u bit / %u ch",
             s_bound->name, sample_rate, bit_depth, channels);
    return ESP_OK;
}

const michi_dac_caps_t *michi_dac_get_caps(void)
{
    /* On classifier error (e.g. a driver without a caps template) the output
     * stays zeroed: model "" and detected=false, the documented no-DAC shape.
     * The classifier itself logs the error loudly. */
    michi_dac_classifier_build(s_bound, s_state, s_board_verified, &s_caps);
    return &s_caps;
}

esp_err_t michi_dac_get_status(michi_dac_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited || s_bound == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return s_bound->ops.get_status(s_bound, (void *)s_bus, status);
}

esp_err_t michi_dac_set_volume(uint8_t volume_0_100)
{
    if (!s_inited || s_bound == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state != MICHI_DAC_STATE_INITIALIZED) {
        ESP_LOGW(TAG, "set_volume before start");
        return ESP_ERR_INVALID_STATE;
    }
    return s_bound->ops.set_volume(s_bound, (void *)s_bus, volume_0_100);
}

esp_err_t michi_dac_set_mute(bool mute)
{
    if (!s_inited || s_bound == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state != MICHI_DAC_STATE_INITIALIZED) {
        ESP_LOGW(TAG, "set_mute before start");
        return ESP_ERR_INVALID_STATE;
    }
    return s_bound->ops.set_mute(s_bound, (void *)s_bus, mute);
}

esp_err_t michi_dac_shutdown(void)
{
    if (!s_inited || s_bound == NULL) {
        return ESP_OK; /* nothing to do */
    }
    esp_err_t err = s_bound->ops.shutdown(s_bound, (void *)s_bus);
    /* Clean semantics: shutdown UNBINDS the driver. The next detect() probes
     * the bus again (init -> detect -> start -> shutdown -> detect -> start).
     * Keeping s_bound here would make detect() early-return and start() fail
     * on the un-initialized bound driver - a permanently broken state. */
    s_bound = NULL;
    s_state = MICHI_DAC_STATE_NONE;
    s_board_verified = false;
    s_bind_reason[0] = '\0';
    ESP_LOGI(TAG, "shutdown: driver unbound, state=NONE");
    return err;
}

void michi_dac_register_hw_id_source(michi_dac_hw_id_fn fn)
{
    s_hw_id_source = fn;
    ESP_LOGI(TAG, "hw-id source %sregistered", fn != NULL ? "" : "un");
}
