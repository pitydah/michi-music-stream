#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "michi_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reusable screen components for Landscape 320x240 UI.
 *
 * Band contract (MS-11): every component receives the band framebuffer
 * (fb_w x fb_h RGB565) plus y_origin, the absolute top row of the band.
 * Layout coordinates stay ABSOLUTE panel rows (320 x 240); the underlying
 * primitives convert to local band coordinates and clip per pixel.
 */

/*!< Longest input string the components copy into their internal buffer */
#define MICHI_UI_COMPONENT_MAX_STR 124

/*!< Status dot radius in pixels */
#define MICHI_UI_STATUS_DOT_R 3

/**
 * @brief Clear a band framebuffer to a background color (fast 32-bit word fill).
 */
void ui_clear_band(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t color);

/**
 * @brief Draw a filled circle of radius @p r centered at absolute (cx, cy).
 */
void michi_ui_draw_circle_filled(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                 uint16_t y_origin, int cx, int cy, int r,
                                 uint16_t color);

/**
 * @brief Draw the standard status dot (radius 3).
 */
void michi_ui_draw_status_dot(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                              uint16_t y_origin, int cx, int cy,
                              uint16_t color);

/**
 * @brief Draw a horizontal line from x to x+w-1 at absolute row y.
 */
void michi_ui_draw_hline(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                         uint16_t y_origin, int x, int y, int w,
                         uint16_t color);

/**
 * @brief Draw the landscape header: Brand "Michi" at (16, 14),
 *        Wi-Fi icon at x=265, Server icon at x=290.
 */
void michi_ui_draw_header_landscape(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                    uint16_t y_origin, const char *brand,
                                    bool wifi_connected, int8_t wifi_rssi,
                                    bool server_connected);

/**
 * @brief Draw the header divider line at y=42 (w=288, x=16..304).
 *        If is_playing is true, the first 28 px are highlighted in ACCENT.
 */
void michi_ui_draw_divider(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                           uint16_t y_origin, bool is_playing);

/**
 * @brief Draw the playback footer at y=211: Speaker icon + volume at left,
 *        format string (e.g. "48 · 16") right-aligned at x=304.
 */
void michi_ui_draw_playback_footer_landscape(uint16_t *fb, uint16_t fb_w,
                                             uint16_t fb_h, uint16_t y_origin,
                                             uint8_t volume,
                                             const char *format_str);

/**
 * @brief Draw the minimal audio footer at y=212: source left, format right-aligned.
 *        No speaker icon, no volume.
 */
void michi_ui_draw_audio_footer_landscape(uint16_t *fb, uint16_t fb_w,
                                          uint16_t fb_h, uint16_t y_origin,
                                          const char *source,
                                          const char *format_str);

/**
 * @brief Draw a 6-digit pairing PIN visually spaced as "XXX XXX" centered
 *        at absolute (x_center, y_center) in MICHI_FONT_PIN (41 px height).
 */
void michi_ui_draw_pin_landscape(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                 uint16_t y_origin, int x_center, int y_center,
                                 const char *pin, uint16_t color);

/**
 * @brief Draw a horizontal row of activity dots centered at (cx, cy).
 */
void michi_ui_draw_activity_dots(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                 uint16_t y_origin, int cx, int cy,
                                 int count, int spacing, uint16_t color);

/**
 * @brief Draw a fine progress bar (h=3..4 px) without heavy outline.
 */
void michi_ui_draw_progress_landscape(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                      uint16_t y_origin, int x, int y,
                                      int w, int h, uint8_t pct);

/**
 * @brief Draw the temporary volume overlay screen.
 */
void michi_ui_draw_volume_overlay(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                  uint16_t y_origin, uint8_t volume);

/**
 * @brief Wrap and draw str inside rect.
 */
int michi_ui_draw_multiline(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                            uint16_t y_origin, const michi_ui_rect_t *rect,
                            michi_ui_font_id_t font, const char *str,
                            michi_ui_align_t align, uint16_t color);

/**
 * @brief Draw a message centered horizontally and vertically at y_center.
 */
void michi_ui_draw_centered_message(uint16_t *fb, uint16_t fb_w,
                                    uint16_t fb_h, uint16_t y_origin,
                                    int y_center, michi_ui_font_id_t font,
                                    const char *str, uint16_t color);

/* Backward-compatible wrappers */
void michi_ui_draw_header(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                          uint16_t y_origin, const char *title);
void michi_ui_draw_footer(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                          uint16_t y_origin, const char *text);
void michi_ui_draw_header_bar(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                              uint16_t y_origin, const char *brand,
                              michi_ui_icon_t right_icon, bool show_icon,
                              uint16_t icon_color);
void michi_ui_draw_playback_footer(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                   uint16_t y_origin, uint8_t volume,
                                   const char *format_str);
void michi_ui_draw_pin(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                       uint16_t y_origin, int y_center, const char *pin,
                       uint16_t color);
void michi_ui_draw_progress(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                            uint16_t y_origin, int x, int y, int w, int h,
                            uint8_t pct);

#ifdef __cplusplus
}
#endif
