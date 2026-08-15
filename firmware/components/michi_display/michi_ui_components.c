#include <stdio.h>
#include <string.h>

#include "michi_ui_components.h"

/*
 * Internal text buffer: MICHI_UI_COMPONENT_MAX_STR input bytes + 3 bytes
 * for the '…' ellipsis ui_ellipsize appends + the NUL.
 */
#define MICHI_UI_TEXT_BUF_BYTES (MICHI_UI_COMPONENT_MAX_STR + 4)

/*!< Absolute cap on wrapped lines (48 > 320 / 8, the smallest em box). */
#define MICHI_UI_MULTILINE_MAX_LINES 48

/* Copy str into a fixed internal buffer, truncated to the component input
 * bound. Both ui_wrap_text and ui_ellipsize MUTATE the buffer they process
 * in place, so the components never wrap/ellipsize the caller's string. */
static void copy_input(char *dst, const char *str)
{
    size_t n;

    if (str == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strnlen(str, MICHI_UI_COMPONENT_MAX_STR);
    memcpy(dst, str, n);
    dst[n] = '\0';
}

/* Single-pixel write with per-pixel band clipping (absolute y). Every
 * component pixel write goes through here: nothing can land outside the
 * band [0, fb_w) x [0, fb_h). */
static void put_px(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                   uint16_t y_origin, int x, int y_abs, uint16_t color)
{
    int ly = y_abs - (int)y_origin;

    if (ly < 0 || ly >= (int)fb_h || x < 0 || x >= (int)fb_w) {
        return;
    }
    fb[(size_t)ly * (size_t)fb_w + (size_t)x] = color;
}

void michi_ui_draw_header(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                          uint16_t y_origin, const char *title)
{
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, MICHI_UI_HEADER_Y,
                          title, michi_ui_font_get(MICHI_FONT_MD),
                          MICHI_UI_TEXT_PRIMARY);
}

void michi_ui_draw_footer(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                          uint16_t y_origin, const char *text)
{
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, MICHI_UI_FOOTER_Y,
                          text, michi_ui_font_get(MICHI_FONT_SM),
                          MICHI_UI_MUTED);
}

void michi_ui_draw_status_dot(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                              uint16_t y_origin, int cx, int cy,
                              uint16_t color)
{
    const int r = MICHI_UI_STATUS_DOT_R;
    int dy;

    for (dy = -r; dy <= r; dy++) {
        int dx;

        for (dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r * r) {
                continue;
            }
            put_px(fb, fb_w, fb_h, y_origin, cx + dx, cy + dy, color);
        }
    }
}

int michi_ui_draw_multiline(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                            uint16_t y_origin, const michi_ui_rect_t *rect,
                            michi_ui_font_id_t font_id, const char *str,
                            michi_ui_align_t align, uint16_t color)
{
    const michi_ui_font_t *font;
    char buf[MICHI_UI_TEXT_BUF_BYTES];
    const char *lines[MICHI_UI_MULTILINE_MAX_LINES];
    int line_h;
    int max_lines;
    int n;
    int y;
    int i;

    if (rect == NULL || str == NULL || rect->w <= 0 || rect->h <= 0) {
        return 0;
    }
    font = michi_ui_font_get(font_id);
    line_h = michi_ui_font_line_height(font);
    max_lines = rect->h / line_h;
    if (max_lines <= 0) {
        return 0;
    }
    if (max_lines > MICHI_UI_MULTILINE_MAX_LINES) {
        max_lines = MICHI_UI_MULTILINE_MAX_LINES;
    }
    copy_input(buf, str);
    n = ui_wrap_text(font, buf, rect->w, lines, max_lines);
    y = rect->y;
    for (i = 0; i < n; i++) {
        ui_draw_text_aligned(fb, fb_w, fb_h, y_origin, rect, y, lines[i],
                             font, color, align);
        y += line_h;
    }
    return n;
}

void michi_ui_draw_progress(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                            uint16_t y_origin, int x, int y, int w, int h,
                            uint8_t pct)
{
    int fill_w;
    int i;

    if (w <= 0 || h <= 0) {
        return;
    }
    if (pct > 100) {
        pct = 100;
    }
    fill_w = (w - 2) * (int)pct / 100;
    if (fill_w < 0) {
        fill_w = 0;
    }

    /* 1 px outline (muted): top/bottom rows plus left/right columns. */
    for (i = 0; i < w; i++) {
        put_px(fb, fb_w, fb_h, y_origin, x + i, y, MICHI_UI_MUTED);
        put_px(fb, fb_w, fb_h, y_origin, x + i, y + h - 1, MICHI_UI_MUTED);
    }
    for (i = 1; i < h - 1; i++) {
        put_px(fb, fb_w, fb_h, y_origin, x, y + i, MICHI_UI_MUTED);
        put_px(fb, fb_w, fb_h, y_origin, x + w - 1, y + i, MICHI_UI_MUTED);
    }

    /* Fill from the left (accent), inside the outline. */
    for (i = 1; i < h - 1; i++) {
        int j;

        for (j = 1; j <= fill_w; j++) {
            put_px(fb, fb_w, fb_h, y_origin, x + j, y + i, MICHI_UI_ACCENT);
        }
    }
}

void michi_ui_draw_centered_message(uint16_t *fb, uint16_t fb_w,
                                    uint16_t fb_h, uint16_t y_origin,
                                    int y_center, michi_ui_font_id_t font_id,
                                    const char *str, uint16_t color)
{
    const michi_ui_font_t *font;
    char buf[MICHI_UI_TEXT_BUF_BYTES];
    int line_h;
    int y;

    if (str == NULL) {
        return;
    }
    font = michi_ui_font_get(font_id);
    copy_input(buf, str);
    (void)ui_ellipsize(font, buf, MICHI_UI_PANEL_W);
    line_h = michi_ui_font_line_height(font);
    y = y_center - line_h / 2;
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, y, buf, font, color);
}

void michi_ui_draw_header_bar(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                              uint16_t y_origin, const char *brand,
                              michi_ui_icon_t right_icon, bool show_icon,
                              uint16_t icon_color)
{
    if (brand != NULL) {
        ui_draw_text(fb, fb_w, fb_h, y_origin, 14, 14, brand,
                     michi_ui_font_get(MICHI_FONT_MD), MICHI_UI_TEXT_PRIMARY);
    }
    if (show_icon) {
        ui_draw_icon(fb, fb_w, fb_h, y_origin, 214, 14, right_icon,
                     MICHI_UI_ICON_SIZE_12, icon_color);
    }
}

void michi_ui_draw_playback_footer(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                   uint16_t y_origin, uint8_t volume,
                                   const char *format_str)
{
    char vol_str[8];
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);

    /* Left: Speaker icon + Volume */
    ui_draw_icon(fb, fb_w, fb_h, y_origin, 16, 286, MICHI_UI_ICON_SPEAKER,
                 MICHI_UI_ICON_SIZE_12, MICHI_UI_TEXT_SECONDARY);
    snprintf(vol_str, sizeof(vol_str), "%u", (unsigned)volume);
    ui_draw_text(fb, fb_w, fb_h, y_origin, 34, 286, vol_str,
                 font_sm, MICHI_UI_TEXT_PRIMARY);

    /* Right: Audio format */
    if (format_str != NULL && format_str[0] != '\0') {
        int fmt_w = ui_text_measure(font_sm, format_str);
        int x = 224 - fmt_w;
        if (x < 100) {
            x = 100;
        }
        ui_draw_text(fb, fb_w, fb_h, y_origin, x, 286, format_str,
                     font_sm, MICHI_UI_TEXT_TERTIARY);
    }
}

void michi_ui_draw_pin(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                       uint16_t y_origin, int y_center, const char *pin,
                       uint16_t color)
{
    char formatted[16];
    const michi_ui_font_t *font_pin = michi_ui_font_get(MICHI_FONT_PIN);
    size_t len;
    int pin_w;
    int x;
    int y;

    if (pin == NULL) {
        return;
    }
    len = strlen(pin);
    if (len == 6) {
        /* Format "123456" as "123 456" */
        formatted[0] = pin[0];
        formatted[1] = pin[1];
        formatted[2] = pin[2];
        formatted[3] = ' ';
        formatted[4] = pin[3];
        formatted[5] = pin[4];
        formatted[6] = pin[5];
        formatted[7] = '\0';
    } else {
        copy_input(formatted, pin);
    }

    pin_w = ui_text_measure(font_pin, formatted);
    x = ((int)fb_w - pin_w) / 2;
    if (x < 0) {
        x = 0;
    }
    y = y_center - (int)font_pin->height / 2;
    ui_draw_text(fb, fb_w, fb_h, y_origin, x, y, formatted, font_pin, color);
}

void michi_ui_draw_activity_dots(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                 uint16_t y_origin, int cx, int cy,
                                 int count, int spacing, uint16_t color)
{
    int total_w;
    int start_x;
    int i;

    if (count <= 0) {
        return;
    }
    total_w = (count - 1) * spacing;
    start_x = cx - total_w / 2;
    for (i = 0; i < count; i++) {
        michi_ui_draw_status_dot(fb, fb_w, fb_h, y_origin, start_x + i * spacing,
                                 cy, color);
    }
}

