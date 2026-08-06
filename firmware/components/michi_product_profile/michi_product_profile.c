/*
 * Dynamic product profile.
 *
 * THE single source of truth for everything the product announces: commercial
 * name, tier, DAC, formats, sample rates, bit depth, connectors, volume, OTA,
 * display, lighting and diagnostics. Later phases (API, mDNS/BLE, screens,
 * sessions) must read THIS profile; no subsystem may duplicate product
 * strings.
 *
 * Everything is DERIVED at runtime from real evidence:
 *  - michi_dac_get_caps(): tier (degraded to DIAGNOSTIC by michi_dac when
 *    !detected || !initialized; with no driver bound the caps are zeroed and
 *    tier reads STANDARD, so refresh() re-raises DIAGNOSTIC on !detected),
 *    DAC identity and silicon capabilities.
 *  - michi_board_get_info(): board model, display geometry.
 *  - michi_board_self_test(): display presence (display_ok).
 * No field is asserted: if there is no evidence, the profile says so.
 *
 * Project restrictions:
 *   - lighting_cat_contour is ALWAYS false: the cat-contour LED strip is NOT
 *     implemented. No GPIO, channel or driver is reserved or announced; the
 *     false field is the only mention.
 *   - sample rates are only {48000} (the system validation baseline). The
 *     silicon max_sample_rate is exposed separately as a capability and is
 *     NOT claimed as supported yet.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_partition.h"

#include "michi_board.h"
#include "michi_dac.h"
#include "michi_product_profile.h"
#include "michi_version.h"

#define MICHI_PROFILE_VALIDATED_SAMPLE_RATE 48000
#define MICHI_PROFILE_VOLUME_MIN 0
#define MICHI_PROFILE_VOLUME_MAX 100

/* Zeroed would read as STANDARD (enum value 0): a get() before any refresh
 * must report diagnostic, never standard. */
static michi_product_profile_t s_profile = {
    .tier = MICHI_PRODUCT_DIAGNOSTIC,
};

static const char *tier_to_name(michi_product_tier_t tier)
{
    switch (tier) {
    case MICHI_PRODUCT_HIFI:
        return "hifi";
    case MICHI_PRODUCT_STANDARD:
        return "standard";
    default:
        return "diagnostic";
    }
}

static void copy_str(char *dst, size_t dst_len, const char *src)
{
    snprintf(dst, dst_len, "%s", src != NULL ? src : "");
}

esp_err_t michi_product_profile_refresh(void)
{
    const michi_dac_caps_t *caps = michi_dac_get_caps();
    const michi_board_info_t *info = michi_board_get_info();
    const michi_board_selftest_t st = michi_board_self_test();

    michi_product_profile_t p = {0};

    /* Tier: delegated to michi_dac, which degrades to DIAGNOSTIC when
     * !detected || !initialized — but ONLY while a driver is bound. With no
     * driver linked (nothing probed on the I2C bus) the classifier returns
     * zeroed caps whose tier field reads STANDARD (enum value 0), so re-raise
     * DIAGNOSTIC here when nothing was detected. */
    p.tier = caps->detected ? caps->tier : MICHI_PRODUCT_DIAGNOSTIC;
    copy_str(p.product_name, sizeof(p.product_name),
             p.tier == MICHI_PRODUCT_HIFI ? "Michi Music Stream HiFi"
                                          : "Michi Music Stream");
    p.audio_available = (p.tier == MICHI_PRODUCT_HIFI ||
                         p.tier == MICHI_PRODUCT_STANDARD) &&
                        caps->initialized;

    copy_str(p.dac_vendor, sizeof(p.dac_vendor), caps->vendor);
    copy_str(p.dac_model, sizeof(p.dac_model), caps->model);
    copy_str(p.dac_board_profile, sizeof(p.dac_board_profile), caps->board_profile);
    p.dac_board_verified = caps->board_verified;

    /* max_sample_rate is the silicon capability (from caps); only
     * {48000} is claimed as validated on this system. */
    p.max_sample_rate = caps->max_sample_rate;
    p.validated_sample_rate = MICHI_PROFILE_VALIDATED_SAMPLE_RATE;
    p.max_bit_depth = caps->max_bit_depth;
    p.channels = caps->channels;
    p.snr_db = caps->snr_db;

    p.supported_codecs_count = 0;
    copy_str(p.supported_codecs[p.supported_codecs_count],
             sizeof(p.supported_codecs[0]), "pcm_s16le");
    p.supported_codecs_count++;
    /* pcm_s24le is a capability claim: the DAC silicon accepts 24-bit samples
     * (audio_available already excludes the diagnostic tier). */
    if (caps->max_bit_depth >= 24) {
        copy_str(p.supported_codecs[p.supported_codecs_count],
                 sizeof(p.supported_codecs[0]), "pcm_s24le");
        p.supported_codecs_count++;
    }

    p.supported_sample_rates_count = 0;
    p.supported_sample_rates[p.supported_sample_rates_count] =
        MICHI_PROFILE_VALIDATED_SAMPLE_RATE;
    p.supported_sample_rates_count++;

    /* Connector derived from DAC caps: "differential_stereo" vs
     * "single_ended_stereo" is a capability statement; the PHYSICAL connector
     * on the unit is pending hardware validation and is not claimed here. */
    copy_str(p.output_connector, sizeof(p.output_connector),
             caps->differential_output ? "differential_stereo"
                                       : "single_ended_stereo");

    p.volume_hardware = caps->hardware_volume;
    p.volume_min = MICHI_PROFILE_VOLUME_MIN;
    p.volume_max = MICHI_PROFILE_VOLUME_MAX;

    /* OTA: A/B partitions present at runtime (ota_0/ota_1, partitions.csv);
     * the OTA service itself lands in phase 13. */
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    p.ota_supported = (it != NULL);
    if (it != NULL) {
        esp_partition_iterator_release(it);
    }

    p.display_present = st.display_ok;
    p.display_width = info->display_width;
    p.display_height = info->display_height;

    /* M5Stack U003 LED strip declared by the project owner; driver in phase 7. */
    p.lighting_status_rgb = true;
    /* Project restriction: the cat-contour LED strip is NOT implemented.
     * Always false; no GPIO, channel or driver is reserved or announced, and
     * this field is the only mention of it. */
    p.lighting_cat_contour = false;

    copy_str(p.firmware_version, sizeof(p.firmware_version), MICHI_FW_VERSION_STR);
    copy_str(p.board_model, sizeof(p.board_model), info->model);

    s_profile = p;
    return ESP_OK;
}

esp_err_t michi_product_profile_init(void)
{
    /* No log here: app_main owns the single consolidated profile line. */
    return michi_product_profile_refresh();
}

const michi_product_profile_t *michi_product_profile_get(void)
{
    return &s_profile;
}

const char *michi_product_profile_tier_name(void)
{
    return tier_to_name(s_profile.tier);
}

esp_err_t michi_product_profile_format_codecs(const michi_product_profile_t *p,
                                              char *buf, size_t buf_len)
{
    if (p == NULL || buf == NULL || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t off = 0;
    buf[0] = '\0';
    for (uint8_t i = 0; i < p->supported_codecs_count && off < buf_len; i++) {
        int n = snprintf(buf + off, buf_len - off, "%s%s",
                         i > 0 ? "," : "", p->supported_codecs[i]);
        if (n <= 0) {
            break;
        }
        off += (size_t)n;
    }
    return ESP_OK;
}

esp_err_t michi_product_profile_format_rates(const michi_product_profile_t *p,
                                             char *buf, size_t buf_len)
{
    if (p == NULL || buf == NULL || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t off = 0;
    buf[0] = '\0';
    for (uint8_t i = 0; i < p->supported_sample_rates_count && off < buf_len; i++) {
        int n = snprintf(buf + off, buf_len - off, "%s%" PRIu32,
                         i > 0 ? "," : "", p->supported_sample_rates[i]);
        if (n <= 0) {
            break;
        }
        off += (size_t)n;
    }
    return ESP_OK;
}
