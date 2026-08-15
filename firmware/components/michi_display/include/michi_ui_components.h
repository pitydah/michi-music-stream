#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "michi_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reusable screen components (UI redesign, phase UI-02).
 *
 * Composite building blocks over the UI-01 primitives: header/footer
 * chrome, the status dot, wrapped multiline text, progress bars and
 * centered messages. Purely presentational and INERT in this phase: no
 * existing screen migrates yet (later phases replace the ad-hoc drawing
 * in michi_display.c with these components).
 *
 * Band contract (MS-11, inherited from michi_board): every component
 * receives the band framebuffer (fb_w x fb_h RGB565) plus y_origin, the
 * absolute top row of the band. Layout coordinates stay ABSOLUTE panel
 * rows (240 x 320); the underlying UI-01 primitives convert to the local
 * band row and clip per pixel, so an element straddling a band boundary
 * is split correctly across two flushes. Components never write outside
 * the band; host tests prove banded rendering is pixel-identical to a
 * full-frame render.
 */

/*!< Panel width in pixels (x spans [0, 240)). */
#define MICHI_UI_PANEL_W 240

/*!< Header title row in absolute panel rows. */
#define MICHI_UI_HEADER_Y 8

/*!< Footer row in absolute panel rows. */
#define MICHI_UI_FOOTER_Y 304

/*!< Status dot radius in pixels. */
#define MICHI_UI_STATUS_DOT_R 3

/*!< Longest input string the components copy into their internal buffer
 *   (128 bytes total: the remaining 4 bytes are reserved for the 3-byte
 *   '…' appended by ui_ellipsize plus the NUL, so a full-length input can
 *   never overflow). Longer inputs are truncated. */
#define MICHI_UI_COMPONENT_MAX_STR 124

/**
 * @brief Draw the screen header: title centered at MICHI_UI_HEADER_Y in
 *        MICHI_UI_FONT_MD, MICHI_UI_TEXT_PRIMARY.
 *
 * Band contract: absolute rows, per-pixel clipping (a title em box is
 * small enough to fit one band, but the text primitive clips anyway).
 */
void michi_ui_draw_header(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                          uint16_t y_origin, const char *title);

/**
 * @brief Draw the screen footer: text centered at MICHI_UI_FOOTER_Y in
 *        MICHI_UI_FONT_SM, MICHI_UI_MUTED.
 *
 * Band contract: absolute rows, per-pixel clipping (same as the header).
 */
void michi_ui_draw_footer(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                          uint16_t y_origin, const char *text);

/**
 * @brief Draw a filled circle of MICHI_UI_STATUS_DOT_R px centered at
 *        absolute (cx, cy). Palette colors come from the caller.
 *
 * Band contract: clipped per pixel, so a dot straddling a band boundary
 * is split correctly across two flushes. Background untouched elsewhere.
 */
void michi_ui_draw_status_dot(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                              uint16_t y_origin, int cx, int cy,
                              uint16_t color);

/**
 * @brief Wrap and draw str inside rect: wraps to rect.w via ui_wrap_text
 *        and draws up to floor(rect.h / line_height) lines, aligned
 *        LEFT/CENTER/RIGHT inside rect, top-down from rect.y.
 *
 * Text color is @p color; background untouched outside the glyph pixels.
 * Returns the number of lines drawn (0 when rect or str is invalid).
 *
 * ui_wrap_text MUTATES the buffer it processes (NUL line breaks), so the
 * component copies str into an internal buffer first: callers keep their
 * string intact and do NOT need to pass a mutable copy. Inputs longer
 * than MICHI_UI_COMPONENT_MAX_STR bytes are truncated.
 *
 * Band contract: absolute rows; lines straddling a band boundary are
 * clipped per pixel by the text primitive.
 */
int michi_ui_draw_multiline(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                            uint16_t y_origin, const michi_ui_rect_t *rect,
                            michi_ui_font_id_t font, const char *str,
                            michi_ui_align_t align, uint16_t color);

/**
 * @brief Draw an outlined progress bar with its top-left at absolute
 *        (x, y): 1 px MICHI_UI_MUTED outline, MICHI_UI_ACCENT fill from
 *        the left. pct is clamped to 0..100.
 *
 * Band contract: clipped per pixel, so a bar straddling a band boundary
 * is split correctly across two flushes.
 */
void michi_ui_draw_progress(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                            uint16_t y_origin, int x, int y, int w, int h,
                            uint8_t pct);

/**
 * @brief Draw a message horizontally centered on the panel and
 *        vertically centered on absolute y_center (the em box top is
 *        y_center - line_height / 2), ellipsized to MICHI_UI_PANEL_W.
 *
 * ui_ellipsize MUTATES the buffer it processes (in-place '…' tail), so
 * the component copies str into an internal buffer first: callers keep
 * their string intact and do NOT need to pass a mutable copy. Inputs
 * longer than MICHI_UI_COMPONENT_MAX_STR bytes are truncated.
 *
 * Band contract: absolute rows, per-pixel clipping by the text
 * primitive; a message straddling a band boundary splits correctly.
 */
void michi_ui_draw_centered_message(uint16_t *fb, uint16_t fb_w,
                                    uint16_t fb_h, uint16_t y_origin,
                                    int y_center, michi_ui_font_id_t font,
                                    const char *str, uint16_t color);

/**
 * @brief Draw the standard header bar: brand text left-aligned at (14, 14),
 *        with optional right status indicator (Wi-Fi / status dot / wave).
 */
void michi_ui_draw_header_bar(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                              uint16_t y_origin, const char *brand,
                              michi_ui_icon_t right_icon, bool show_icon,
                              uint16_t icon_color);

/**
 * @brief Draw the playback footer: speaker icon + volume at left (x=16, y=286),
 *        format string (e.g. "48/16" or "48 kHz") right-aligned at x=224.
 */
void michi_ui_draw_playback_footer(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                   uint16_t y_origin, uint8_t volume,
                                   const char *format_str);

/**
 * @brief Draw a 6-digit pairing PIN visually spaced as "XXX XXX" centered
 *        at absolute y_center in MICHI_FONT_PIN.
 */
void michi_ui_draw_pin(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                       uint16_t y_origin, int y_center, const char *pin,
                       uint16_t color);

/**
 * @brief Draw a horizontal row of status/activity dots centered at (cx, cy).
 */
void michi_ui_draw_activity_dots(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                 uint16_t y_origin, int cx, int cy,
                                 int count, int spacing, uint16_t color);

#ifdef __cplusplus
}
#endif
