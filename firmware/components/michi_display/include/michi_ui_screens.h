#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "michi_state.h"
#include "michi_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Screen rendering context: holds all live parameters passed to the UI
 *        layer for drawing any given state.
 */
typedef struct michi_ui_screen_ctx {
    michi_state_t state;           /*!< Product FSM state */
    const char *title;             /*!< Now Playing track title */
    const char *artist;            /*!< Now Playing artist */
    const char *source;            /*!< Now Playing source label */
    const char *pairing_pin;       /*!< 6-digit pairing PIN */
    uint8_t volume;                /*!< Volume (0..100) */
    uint32_t sample_rate;          /*!< Sample rate in Hz (e.g. 48000) */
    uint8_t bit_depth;             /*!< Bit depth (e.g. 16) */
    uint32_t last_error;           /*!< Last captured error code */
    bool wifi_connected;           /*!< Wi-Fi connected flag */
    bool server_connected;         /*!< Server connected flag */
    uint8_t update_pct;            /*!< OTA progress percentage (0..100) */
    bool show_diagnostics;         /*!< Force diagnostics view */
} michi_ui_screen_ctx_t;

/**
 * @brief Top-level screen dispatcher: draws the screen matching ctx->state
 *        (or diagnostics if ctx->show_diagnostics is true) for the current band.
 */
void michi_ui_render_screen(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                            uint16_t y_origin, const michi_ui_screen_ctx_t *ctx);

/* Specific screen drawing functions (also exposed for tests and modularity) */
void michi_ui_draw_screen_boot(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin);
void michi_ui_draw_screen_ready(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin, const michi_ui_screen_ctx_t *ctx);
void michi_ui_draw_screen_unprovisioned(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin);
void michi_ui_draw_screen_provisioning(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin);
void michi_ui_draw_screen_connecting(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin, const char *ssid);
void michi_ui_draw_screen_pairing(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin, const char *pin);
void michi_ui_draw_screen_session_pending(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin);
void michi_ui_draw_screen_buffering(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin, const michi_ui_screen_ctx_t *ctx);
void michi_ui_draw_screen_playing(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin, const michi_ui_screen_ctx_t *ctx);
void michi_ui_draw_screen_paused(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin, const michi_ui_screen_ctx_t *ctx);
void michi_ui_draw_screen_updating(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin, uint8_t pct);
void michi_ui_draw_screen_recoverable_error(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin, uint32_t error_code);
void michi_ui_draw_screen_fatal_error(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin, uint32_t error_code);
void michi_ui_draw_screen_diagnostics(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin, const michi_ui_screen_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
