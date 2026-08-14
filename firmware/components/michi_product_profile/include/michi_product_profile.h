#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "michi_dac_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Dynamic product profile: the single source of truth for everything
 *        the product announces (commercial name, tier, DAC, formats, sample
 *        rates, bit depth, connectors, volume, OTA, display, lighting and
 *        diagnostics).
 *
 * Derived at runtime by michi_product_profile_init()/refresh() from real
 * evidence: michi_dac_get_caps() + michi_board_get_info() +
 * michi_board_self_test(). Later phases (API, mDNS/BLE, screens, sessions)
 * must read THIS profile and never duplicate product strings.
 *
 * Honesty rules baked in:
 * - tier comes from michi_dac caps, degraded to DIAGNOSTIC when
 *   !detected || !initialized; with NO driver bound the classifier returns
 *   zeroed caps (tier reads STANDARD), so michi_product_profile re-raises
 *   DIAGNOSTIC on !detected.
 * - supported_sample_rates are only {48000} (system validation baseline);
 *   max_sample_rate is the silicon capability and is NOT claimed as
 *   supported yet.
 * - lighting_cat_contour is ALWAYS false: the cat-contour LED strip is NOT
 *   implemented (project restriction - no GPIO/channel/driver is reserved or
 *   announced; the false field is the only mention).
 * - output_connector is derived from DAC caps (differential vs single-ended);
 *   the physical connector on the unit is pending hardware validation.
 */
typedef struct {
    char product_name[32];            /* Product name derived from tier in michi_product_profile.c (single source) */
    michi_product_tier_t tier;        /* from michi_dac caps: STANDARD|HIFI|DIAGNOSTIC */
    bool audio_available;             /* tier != DIAGNOSTIC && dac.initialized */
    /* DAC */
    char dac_vendor[24];              /* DAC vendor, from caps */
    char dac_model[32];               /* DAC model, from caps */
    char dac_board_profile[32];       /* DAC board profile, from caps */
    bool dac_board_verified;          /* true only when bound by a real probe */
    uint32_t max_sample_rate;         /* silicon capability (from caps) */
    uint32_t validated_sample_rate;   /* 48000: system validation baseline */
    uint8_t max_bit_depth;            /* silicon capability (from caps; a
                                         24-bit silicon capability is NOT
                                         implemented - S24 retired, MS-08) */
    uint8_t validated_bit_depth;      /* 16: system validation baseline (meta 1) */
    uint8_t channels;                 /* from caps */
    uint16_t snr_db;                  /* from caps */
    char supported_codecs[1][16];     /* "pcm_s16le" only. pcm_s24le was
                                         RETIRED (MS-08): the DAC silicon
                                         accepts 24-bit samples but no S24
                                         path is implemented or announced -
                                         never advertised by the profile */
    uint8_t supported_codecs_count;
    uint32_t supported_sample_rates[4]; /* {48000}: validated baseline */
    uint8_t supported_sample_rates_count;
    /* Output */
    char output_connector[24];        /* "differential_stereo" | "single_ended_stereo" (from caps; physical connector pending HW validation) */
    bool volume_hardware;             /* caps.hardware_volume */
    uint8_t volume_min;               /* 0 */
    uint8_t volume_max;               /* 100 */
    /* Platform */
    bool ota_supported;               /* true: A/B partitions present (service in phase 13) */
    bool display_present;             /* from board self test (display_ok) */
    uint16_t display_width;           /* from board info */
    uint16_t display_height;          /* from board info */
    bool lighting_status_rgb;         /* true: M5Stack U003 present (driver in phase 7) */
    bool lighting_cat_contour;        /* ALWAYS false: cat contour LED strip NOT implemented */
    /* Diagnostics */
    char firmware_version[16];        /* MICHI_FW_VERSION_STR */
    char build_date[16];              /* MICHI_FW_BUILD_DATE (announced by the API, phase 4) */
    char board_model[32];             /* from board info */
    char api_version[8];              /* HTTP API contract version announced by michi_http ("v1-lite"); mDNS TXT (phase 9) reads it from here - no duplicated strings */
} michi_product_profile_t;

/**
 * @brief Canonical product capability flags - THE single source of truth
 *        for the boolean feature surface (MS-08, P0-01 hardening).
 *
 * The discovery announce (michi_discovery) and GET /server/info
 * (michi_http canonical_json.c) BOTH read this table; no subsystem may
 * duplicate a capability literal. A flag is true only while its handler
 * is implemented with a positive test:
 *  - session/heartbeat/volume: implemented (MS-07/MS-08);
 *  - diagnostics: implemented (its response shape is not frozen by the
 *    contract);
 *  - now_playing: the certified payload still answers 501 NOT_IMPLEMENTED;
 *  - ota: the A/B partitions exist, but the OTA service lands in phase 13
 *    (501).
 *
 * The announce carries ONLY the canonical group {heartbeat, session,
 * volume}; the extended flags (now_playing/diagnostics/ota) belong to
 * /server/info, not to the announce.
 */
typedef struct {
    bool session;     /*!< session feature implemented (MS-07) */
    bool heartbeat;   /*!< heartbeat lease implemented (MS-08) */
    bool volume;      /*!< volume via the session PATCH, implemented */
    bool now_playing; /*!< certified payload answers 501 NOT_IMPLEMENTED */
    bool diagnostics; /*!< implemented */
    bool ota;         /*!< 501 NOT_IMPLEMENTED (service lands in phase 13) */
} michi_product_capabilities_t;

/**
 * @brief Get the canonical capability flags (single source of truth).
 *
 * Pure C, no ESP-IDF runtime dependency: the firmware components and the
 * host-side tests compile the SAME source (capabilities.c).
 *
 * @return Pointer to the immutable canonical table; never NULL.
 */
const michi_product_capabilities_t *michi_product_profile_capabilities(void);

/**
 * @brief Build the profile once and cache it (refresh(); the consolidated
 *        summary log lives in app_main).
 *
 * Call after michi_dac init/detect/start and michi_board init/self test.
 *
 * @return Always ESP_OK with the current evidence sources (caps/info/self-test
 *         cannot fail); kept as esp_err_t for future extensibility.
 */
esp_err_t michi_product_profile_init(void);

/**
 * @brief Re-derive the profile from the current caps/board evidence.
 *
 * No duplicated state: rebuilt from michi_dac_get_caps(),
 * michi_board_get_info() and michi_board_self_test() (display only).
 *
 * @note Single-writer contract: refresh() must be called from one task
 *       (boot). get() returns a snapshot pointer valid until the next
 *       refresh; phase-12 consumers (API/mDNS) must define snapshot
 *       semantics before concurrent refresh is introduced.
 *
 * @return ESP_OK.
 */
esp_err_t michi_product_profile_refresh(void);

/**
 * @brief Get the current (cached) profile.
 *
 * @return Pointer to the profile struct; valid until the next refresh()
 *         (single-writer contract, see refresh()).
 */
const michi_product_profile_t *michi_product_profile_get(void);

/**
 * @brief Format supported_codecs as a bounded "a,b,c" list.
 *
 * @param p       Profile to format.
 * @param buf     Output buffer (always NUL-terminated when buf_len > 0).
 * @param buf_len Size of buf.
 * @return ESP_OK; ESP_ERR_INVALID_ARG on NULL args or zero-length buffer.
 */
esp_err_t michi_product_profile_format_codecs(const michi_product_profile_t *p,
                                              char *buf, size_t buf_len);

/**
 * @brief Format supported_sample_rates as a bounded "a,b,c" list.
 *
 * @param p       Profile to format.
 * @param buf     Output buffer (always NUL-terminated when buf_len > 0).
 * @param buf_len Size of buf.
 * @return ESP_OK; ESP_ERR_INVALID_ARG on NULL args or zero-length buffer.
 */
esp_err_t michi_product_profile_format_rates(const michi_product_profile_t *p,
                                             char *buf, size_t buf_len);

/**
 * @brief Short tier name for logs/API: "hifi" | "standard" | "diagnostic".
 *
 * @return Static string; never NULL.
 */
const char *michi_product_profile_tier_name(void);

#ifdef __cplusplus
}
#endif
