#include <stdio.h>
#include <string.h>

#include "michi_ui_components.h"

/*
 * Internal text buffer: MICHI_UI_COMPONENT_MAX_STR input bytes + 3 bytes
 * for the '…' ellipsis ui_ellipsize appends + the NUL.
 */
#define MICHI_UI_TEXT_BUF_BYTES (MICHI_UI_COMPONENT_MAX_STR + 4)

/*!< Absolute cap on wrapped lines. */
#define MICHI_UI_MULTILINE_MAX_LINES 48

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

static void put_px(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                   uint16_t y_origin, int x, int y_abs, uint16_t color)
{
    int ly = y_abs - (int)y_origin;

    if (ly < 0 || ly >= (int)fb_h || x < 0 || x >= (int)fb_w) {
        return;
    }
    fb[(size_t)ly * (size_t)fb_w + (size_t)x] = color;
}

void ui_clear_band(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t color)
{
    if (fb == NULL || fb_w == 0 || fb_h == 0) {
        return;
    }
    uint32_t c32 = ((uint32_t)color << 16) | (uint32_t)color;
    uint32_t *fb32 = (uint32_t *)fb;
    size_t count32 = ((size_t)fb_w * fb_h) / 2;
    for (size_t i = 0; i < count32; i++) {
        fb32[i] = c32;
    }
}

void michi_ui_draw_circle_filled(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                 uint16_t y_origin, int cx, int cy, int r,
                                 uint16_t color)
{
    int dy;
    for (dy = -r; dy <= r; dy++) {
        int dx;
        for (dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r) {
                put_px(fb, fb_w, fb_h, y_origin, cx + dx, cy + dy, color);
            }
        }
    }
}

void michi_ui_draw_status_dot(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                              uint16_t y_origin, int cx, int cy,
                              uint16_t color)
{
    michi_ui_draw_circle_filled(fb, fb_w, fb_h, y_origin, cx, cy, MICHI_UI_STATUS_DOT_R, color);
}

void michi_ui_draw_hline(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                         uint16_t y_origin, int x, int y, int w,
                         uint16_t color)
{
    int ly = y - (int)y_origin;
    if (ly < 0 || ly >= (int)fb_h || w <= 0) {
        return;
    }
    int x0 = x < 0 ? 0 : x;
    int x1 = (x + w > (int)fb_w) ? (int)fb_w : (x + w);
    for (int px = x0; px < x1; px++) {
        fb[(size_t)ly * (size_t)fb_w + (size_t)px] = color;
    }
}

void michi_ui_draw_header_landscape(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                    uint16_t y_origin, const char *brand,
                                    bool wifi_connected, int8_t wifi_rssi,
                                    bool server_connected)
{
    (void)wifi_rssi;
    const char *name = brand != NULL ? brand : MICHI_UI_STR_BRAND;
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);

    /* Brand left-aligned at (16, 14) */
    ui_draw_text(fb, fb_w, fb_h, y_origin, 16, 14, name, font_sm, MICHI_UI_TEXT_PRIMARY);

    /* Wi-Fi Icon at x=265, y=14, size 16 */
    uint16_t wifi_color = wifi_connected ? MICHI_UI_TEXT_SECONDARY : MICHI_UI_MUTED;
    michi_ui_draw_icon(fb, fb_w, fb_h, y_origin, 265, 14, MICHI_ICON_WIFI, 16, wifi_color);

    /* Server Icon at x=290, y=14, size 16 */
    uint16_t srv_color = server_connected ? MICHI_UI_SUCCESS : MICHI_UI_MUTED;
    michi_ui_draw_icon(fb, fb_w, fb_h, y_origin, 290, 14, MICHI_ICON_SERVER, 16, srv_color);
}

void michi_ui_draw_divider(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                           uint16_t y_origin, bool is_playing)
{
    /* Full subtle line across x=16..304 (w=288) at y=42 */
    michi_ui_draw_hline(fb, fb_w, fb_h, y_origin, 16, 42, 288, MICHI_UI_SURFACE_ELEVATED);

    /* During PLAYING, the first 28 px are highlighted in ACCENT */
    if (is_playing) {
        michi_ui_draw_hline(fb, fb_w, fb_h, y_origin, 16, 42, 28, MICHI_UI_ACCENT);
    }
}

void michi_ui_draw_audio_footer_landscape(uint16_t *fb, uint16_t fb_w,
                                          uint16_t fb_h, uint16_t y_origin,
                                          const char *source,
                                          const char *format_str)
{
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);

    /* Left: source label (Device Truth) */
    if (source != NULL && source[0] != '\0') {
        ui_draw_text(fb, fb_w, fb_h, y_origin, 18, 212, source, font_sm,
                     MICHI_UI_TEXT_TERTIARY);
    }

    /* Right: format string right-aligned to x=302 */
    if (format_str != NULL && format_str[0] != '\0') {
        int fmt_w = ui_text_measure(font_sm, format_str);
        int x = 302 - fmt_w;
        if (x < 18) {
            x = 18;
        }
        ui_draw_text(fb, fb_w, fb_h, y_origin, x, 212, format_str, font_sm,
                     MICHI_UI_TEXT_TERTIARY);
    }
}

void michi_ui_draw_playback_footer_landscape(uint16_t *fb, uint16_t fb_w,
                                             uint16_t fb_h, uint16_t y_origin,
                                             uint8_t volume,
                                             const char *format_str)
{
    char vol_str[8];
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);

    /* Left: Speaker icon 16x16 at (16, 210) + Volume digits at (36, 212) */
    michi_ui_draw_icon(fb, fb_w, fb_h, y_origin, 16, 210, MICHI_ICON_SPEAKER, 16,
                       MICHI_UI_TEXT_SECONDARY);

    snprintf(vol_str, sizeof(vol_str), "%u", (unsigned)volume);
    ui_draw_text(fb, fb_w, fb_h, y_origin, 36, 212, vol_str, font_sm,
                 MICHI_UI_TEXT_PRIMARY);

    /* Right: Format string right-aligned to x=304 */
    if (format_str != NULL && format_str[0] != '\0') {
        int fmt_w = ui_text_measure(font_sm, format_str);
        int x = 304 - fmt_w;
        if (x < 120) {
            x = 120;
        }
        ui_draw_text(fb, fb_w, fb_h, y_origin, x, 212, format_str, font_sm,
                     MICHI_UI_TEXT_TERTIARY);
    }
}

void michi_ui_draw_pin_landscape(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                 uint16_t y_origin, int x_center, int y_center,
                                 const char *pin, uint16_t color)
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
        /* Format "739412" as "739 412" */
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
    x = x_center - pin_w / 2;
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
    if (count <= 0) {
        return;
    }
    int total_w = (count - 1) * spacing;
    int start_x = cx - total_w / 2;
    for (int i = 0; i < count; i++) {
        michi_ui_draw_circle_filled(fb, fb_w, fb_h, y_origin, start_x + i * spacing,
                                    cy, 2, color);
    }
}

void michi_ui_draw_progress_landscape(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                      uint16_t y_origin, int x, int y,
                                      int w, int h, uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }
    /* Background rail in MUTED */
    for (int row = 0; row < h; row++) {
        michi_ui_draw_hline(fb, fb_w, fb_h, y_origin, x, y + row, w, MICHI_UI_MUTED);
    }
    /* Filled portion in ACCENT */
    int fill_w = (w * (int)pct) / 100;
    if (fill_w > 0) {
        for (int row = 0; row < h; row++) {
            michi_ui_draw_hline(fb, fb_w, fb_h, y_origin, x, y + row, fill_w, MICHI_UI_ACCENT);
        }
    }
}

void michi_ui_draw_volume_overlay(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                  uint16_t y_origin, uint8_t volume)
{
    char vol_str[8];
    const michi_ui_font_t *font_pin = michi_ui_font_get(MICHI_FONT_PIN);
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);

    /* Large volume number centered (PIN font ~41px) at y=80 */
    snprintf(vol_str, sizeof(vol_str), "%u", (unsigned)volume);
    int num_w = ui_text_measure(font_pin, vol_str);
    int num_x = (320 - num_w) / 2;
    ui_draw_text(fb, fb_w, fb_h, y_origin, num_x, 80, vol_str, font_pin,
                 MICHI_UI_TEXT_PRIMARY);

    /* Progress bar at x=40, y=145, w=240, h=3 */
    michi_ui_draw_progress_landscape(fb, fb_w, fb_h, y_origin, 40, 145, 240, 3, volume);

    /* Label "volumen" centered at y=170 in SM, TERTIARY */
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 170, MICHI_UI_STR_VOLUME_TITLE,
                          font_sm, MICHI_UI_TEXT_TERTIARY);
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
    if (n <= 0) {
        return 0;
    }

    y = rect->y;
    for (i = 0; i < n; i++) {
        ui_draw_text_aligned(fb, fb_w, fb_h, y_origin, rect, y, lines[i],
                             font, color, align);
        y += line_h;
    }
    return n;
}

void michi_ui_draw_centered_message(uint16_t *fb, uint16_t fb_w,
                                    uint16_t fb_h, uint16_t y_origin,
                                    int y_center, michi_ui_font_id_t font_id,
                                    const char *str, uint16_t color)
{
    const michi_ui_font_t *font;
    char buf[MICHI_UI_TEXT_BUF_BYTES];
    int y_top;

    if (str == NULL) {
        return;
    }
    font = michi_ui_font_get(font_id);
    copy_input(buf, str);
    (void)ui_ellipsize(font, buf, (int)fb_w - 32);
    y_top = y_center - (int)font->height / 2;
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, y_top, buf, font, color);
}

/* Backward compatibility */
void michi_ui_draw_header(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                          uint16_t y_origin, const char *title)
{
    michi_ui_draw_header_landscape(fb, fb_w, fb_h, y_origin, title, true, -50, false);
}

void michi_ui_draw_footer(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                          uint16_t y_origin, const char *text)
{
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 211,
                          text, michi_ui_font_get(MICHI_FONT_SM),
                          MICHI_UI_MUTED);
}

void michi_ui_draw_header_bar(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                              uint16_t y_origin, const char *brand,
                              michi_ui_icon_t right_icon, bool show_icon,
                              uint16_t icon_color)
{
    (void)right_icon;
    (void)show_icon;
    (void)icon_color;
    michi_ui_draw_header_landscape(fb, fb_w, fb_h, y_origin, brand, true, -50, false);
}

void michi_ui_draw_playback_footer(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                   uint16_t y_origin, uint8_t volume,
                                   const char *format_str)
{
    michi_ui_draw_playback_footer_landscape(fb, fb_w, fb_h, y_origin, volume, format_str);
}

void michi_ui_draw_pin(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                       uint16_t y_origin, int y_center, const char *pin,
                       uint16_t color)
{
    michi_ui_draw_pin_landscape(fb, fb_w, fb_h, y_origin, 160, y_center, pin, color);
}

void michi_ui_draw_progress(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                            uint16_t y_origin, int x, int y, int w, int h,
                            uint8_t pct)
{
    michi_ui_draw_progress_landscape(fb, fb_w, fb_h, y_origin, x, y, w, h, pct);
}
