#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque DAC driver descriptor. Declared by the driver, registered in
 *        the static registry (dac_registry.c).
 */
typedef struct michi_dac_driver michi_dac_driver_t;

typedef struct {
    bool present; bool i2c_ok; bool pll_locked;
    bool clocks_ok;
    bool error_flag; uint8_t volume_0_100; bool muted;
} michi_dac_status_t;

typedef esp_err_t (*michi_dac_probe_fn)(const michi_dac_driver_t *drv, void *bus_ctx);
typedef esp_err_t (*michi_dac_init_fn)(const michi_dac_driver_t *drv, void *bus_ctx);
typedef esp_err_t (*michi_dac_configure_fn)(const michi_dac_driver_t *drv, void *bus_ctx,
                                            uint32_t sample_rate, uint8_t bit_depth,
                                            uint8_t channels);
typedef esp_err_t (*michi_dac_set_volume_fn)(const michi_dac_driver_t *drv, void *bus_ctx,
                                             uint8_t volume_0_100);
typedef esp_err_t (*michi_dac_set_mute_fn)(const michi_dac_driver_t *drv, void *bus_ctx,
                                           bool mute);
typedef esp_err_t (*michi_dac_get_status_fn)(const michi_dac_driver_t *drv, void *bus_ctx,
                                             michi_dac_status_t *status);
typedef esp_err_t (*michi_dac_shutdown_fn)(const michi_dac_driver_t *drv, void *bus_ctx);

typedef struct michi_dac_driver_ops {
    michi_dac_probe_fn probe; michi_dac_init_fn init; michi_dac_configure_fn configure;
    michi_dac_set_volume_fn set_volume; michi_dac_set_mute_fn set_mute;
    michi_dac_get_status_fn get_status; michi_dac_shutdown_fn shutdown;
} michi_dac_driver_ops_t;

typedef enum { MICHI_PRODUCT_STANDARD, MICHI_PRODUCT_HIFI, MICHI_PRODUCT_DIAGNOSTIC } michi_product_tier_t;

typedef struct {
    char vendor[24]; char model[32]; char board_profile[32];
    uint32_t max_sample_rate; uint8_t max_bit_depth; uint8_t channels; uint16_t snr_db;
    bool detected; bool initialized; bool board_verified;
    bool software_control; bool hardware_volume; bool hardware_mute;
    bool differential_output; bool headphone_output; bool requires_mclk;
    michi_product_tier_t tier;
} michi_dac_caps_t;

/**
 * @brief DAC lifecycle state. NONE -> DETECTED -> INITIALIZED.
 *
 * - NONE: no driver bound (michi_dac_init() done, detect() not called or
 *   failed, or michi_dac_shutdown() unbound the driver).
 * - DETECTED: a driver is bound (probe ACK + sanity, or explicit NVS profile).
 * - INITIALIZED: driver init() + configure() succeeded.
 *
 * The full cycle init -> detect -> start -> shutdown -> detect -> start is
 * supported: shutdown() unbinds the driver and returns to NONE, so the next
 * detect() probes again.
 */
typedef enum {
    MICHI_DAC_STATE_NONE = 0,
    MICHI_DAC_STATE_DETECTED,
    MICHI_DAC_STATE_INITIALIZED,
} michi_dac_state_t;

struct michi_dac_driver {
    const char *name; const char *board_profile; bool self_detectable;
    const michi_dac_caps_t *template; /* silicon caps template, declared by the driver */
    michi_dac_driver_ops_t ops;
};

#ifdef __cplusplus
}
#endif
