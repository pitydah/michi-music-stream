#pragma once

#include <stdint.h>

#include "michi_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Text API over the bitmap typography system (UI redesign, phase
 *        UI-01).
 *
 * Conventions:
 * - All layout math is in PIXELS (measure/wrap/ellipsize take max widths in
 *   px - no magic character limits).
 * - Draw calls receive ABSOLUTE panel coordinates plus y_origin (the band's
 *   absolute top row, MS-11 band contract): pixels outside the band
 *   [0, fb_h) in local Y or outside [0, fb_w) are clipped per pixel, so a
 *   row straddling a band boundary is split correctly.
 * - ui_wrap_text() and ui_ellipsize() MUTATE the input buffer in place
 *   (NUL line breaks / trailing ellipsis); callers own a writable buffer.
 * - Text is UTF-8; only ASCII and '…' are decoded (see michi_ui_fonts.h).
 */

/**
 * @brief Measure a string's width in pixels.
 */
int ui_text_measure(const michi_ui_font_t *font, const char *str);

/**
 * @brief Wrap text to a pixel width, breaking at word boundaries (spaces).
 *
 * Mutates str in place: each break point (space or newline) is replaced
 * with '\0'; out_lines receives pointers to the NUL-terminated lines.
 * A word wider than max_w is NOT cut mid-word: it is kept intact as its
 * own over-wide line (callers ellipsize long single words via
 * ui_ellipsize). Empty lines are never emitted. Newlines ('\n') in the
 * input are honored as explicit breaks. Lines are filled until max_lines
 * is reached; any remaining text is dropped.
 *
 * @param font      Font used for measuring.
 * @param str       Writable NUL-terminated input (modified).
 * @param max_w     Maximum line width in pixels (> 0).
 * @param out_lines Output array of at least max_lines pointers.
 * @param max_lines Maximum number of lines (>= 1).
 * @return Number of lines filled (0 for empty input, <= max_lines).
 */
int ui_wrap_text(const michi_ui_font_t *font, char *str, int max_w,
                 const char **out_lines, int max_lines);

/**
 * @brief Truncate str so it fits max_w, replacing the tail with '…'
 *        (three dots when the font lacks the glyph, e.g. PIN uses '.').
 *
 * Prefers a word boundary when that keeps at least half of the hard pixel
 * cut (no mid-word cut "when avoidable"); a word longer than half the line
 * is cut at the pixel boundary instead. If even the ellipsis alone does
 * not fit, the string is hard-truncated without one.
 *
 * The buffer must have room for 3 extra bytes ('…' is UTF-8).
 *
 * @return str (modified in place).
 */
char *ui_ellipsize(const michi_ui_font_t *font, char *str, int max_w);

/**
 * @brief Draw text left-aligned at absolute position (x, y) where y is the
 *        TOP of the em box, clipped to the band.
 */
void ui_draw_text(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                  uint16_t y_origin, int x, int y, const char *str,
                  const michi_ui_font_t *font, uint16_t color);

/**
 * @brief Draw text centered horizontally in the panel width [0, fb_w).
 */
void ui_draw_text_centered(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                           uint16_t y_origin, int y, const char *str,
                           const michi_ui_font_t *font, uint16_t color);

/**
 * @brief Draw text aligned inside a rectangle (absolute panel coords) at
 *        the given top row (y = top of the em box).
 */
void ui_draw_text_aligned(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                          uint16_t y_origin, const michi_ui_rect_t *box,
                          int y, const char *str,
                          const michi_ui_font_t *font, uint16_t color,
                          michi_ui_align_t align);

/**
 * @brief Draw NUL-terminated lines top-down at absolute x/y with a line
 *        pitch in pixels (y = top of the first line's em box).
 */
void ui_draw_multiline(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                       uint16_t y_origin, const char *const *lines,
                       int count, int x, int y, int line_h,
                       const michi_ui_font_t *font, uint16_t color);

#ifdef __cplusplus
}
#endif
