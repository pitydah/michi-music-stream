#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "michi_ui.h"
#include "michi_ui_components.h"
#include "michi_ui_screens.h"
#include "michi_ui_strings.h"
#include "michi_version.h"

static void format_audio_spec(char *dst, size_t dst_cap, uint32_t sample_rate, uint8_t bit_depth)
{
    if (sample_rate == 0) {
        snprintf(dst, dst_cap, "48 · 16");
    } else {
        snprintf(dst, dst_cap, "%" PRIu32 " · %u", sample_rate / 1000u, (unsigned)bit_depth);
    }
}

void michi_ui_draw_screen_boot(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin)
{
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);

    /* Michi Cat emblem 48x48 centered at cx=160, y=32 */
    michi_ui_draw_icon(fb, fb_w, fb_h, y_origin, (320 - 48) / 2, 32, MICHI_ICON_CAT, 48, MICHI_UI_ACCENT);

    /* Brand "michi" centered at y=92 in LG font */
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 92, MICHI_UI_STR_BRAND_LOWER, font_lg, MICHI_UI_TEXT_PRIMARY);

    /* Subtitle "iniciando" centered at y=128 in SM font */
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 128, MICHI_UI_STR_STARTING, font_sm, MICHI_UI_TEXT_TERTIARY);

    /* 3 Activity dots centered at cx=160, y=162 */
    michi_ui_draw_activity_dots(fb, fb_w, fb_h, y_origin, 160, 162, 3, 10, MICHI_UI_ACCENT);
}

void michi_ui_draw_screen_unprovisioned(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin)
{
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);

    michi_ui_draw_header_landscape(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND, false, 0, false);
    michi_ui_draw_divider(fb, fb_w, fb_h, y_origin, false);

    /* Physical button icon 32x32 at (40, 95) */
    michi_ui_draw_icon(fb, fb_w, fb_h, y_origin, 40, 95, MICHI_ICON_BUTTON, 32, MICHI_UI_ACCENT);

    /* Title & Hint at x=95 */
    ui_draw_text(fb, fb_w, fb_h, y_origin, 95, 85, MICHI_UI_STR_SETUP_TITLE, font_lg, MICHI_UI_TEXT_PRIMARY);

    michi_ui_rect_t r = { .x = 95, .y = 120, .w = 205, .h = 50 };
    michi_ui_draw_multiline(fb, fb_w, fb_h, y_origin, &r, MICHI_FONT_SM,
                            MICHI_UI_STR_SETUP_HINT, MICHI_UI_ALIGN_LEFT, MICHI_UI_TEXT_SECONDARY);
}

void michi_ui_draw_screen_ready(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin,
                                const michi_ui_screen_ctx_t *ctx)
{
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);

    bool wifi = ctx != NULL ? ctx->wifi_connected : true;
    bool srv = ctx != NULL ? ctx->server_connected : false;
    int8_t rssi = ctx != NULL ? ctx->wifi_rssi : -50;

    michi_ui_draw_header_landscape(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND, wifi, rssi, srv);
    michi_ui_draw_divider(fb, fb_w, fb_h, y_origin, false);

    /* Asymmetric 2-column composition */
    /* Left: Michi Cat 48x48 at (55, 88) */
    michi_ui_draw_icon(fb, fb_w, fb_h, y_origin, 55, 88, MICHI_ICON_CAT, 48, MICHI_UI_TEXT_PRIMARY);

    /* Right: Title & Subtitle at x=135 */
    ui_draw_text(fb, fb_w, fb_h, y_origin, 135, 90, MICHI_UI_STR_READY, font_lg, MICHI_UI_TEXT_PRIMARY);
    ui_draw_text(fb, fb_w, fb_h, y_origin, 135, 126, MICHI_UI_STR_WAITING_PLAYBACK, font_sm, MICHI_UI_TEXT_SECONDARY);
}

void michi_ui_draw_screen_provisioning(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin)
{
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);

    michi_ui_draw_header_landscape(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND, false, 0, false);
    michi_ui_draw_divider(fb, fb_w, fb_h, y_origin, false);

    /* Wi-Fi icon 32x32 at (45, 95) */
    michi_ui_draw_icon(fb, fb_w, fb_h, y_origin, 45, 95, MICHI_ICON_WIFI, 32, MICHI_UI_ACCENT);

    /* Text block at x=100 */
    ui_draw_text(fb, fb_w, fb_h, y_origin, 100, 85, MICHI_UI_STR_PROV_TITLE, font_lg, MICHI_UI_TEXT_PRIMARY);

    michi_ui_rect_t r = { .x = 100, .y = 120, .w = 200, .h = 60 };
    michi_ui_draw_multiline(fb, fb_w, fb_h, y_origin, &r, MICHI_FONT_SM,
                            MICHI_UI_STR_PROV_HINT, MICHI_UI_ALIGN_LEFT, MICHI_UI_TEXT_SECONDARY);
}

void michi_ui_draw_screen_connecting(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin,
                                    const char *ssid)
{
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);

    michi_ui_draw_header_landscape(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND, false, 0, false);
    michi_ui_draw_divider(fb, fb_w, fb_h, y_origin, false);

    /* Wi-Fi icon 32x32 at (45, 95) in INFO */
    michi_ui_draw_icon(fb, fb_w, fb_h, y_origin, 45, 95, MICHI_ICON_WIFI, 32, MICHI_UI_INFO);

    /* Text at x=100 */
    ui_draw_text(fb, fb_w, fb_h, y_origin, 100, 88, MICHI_UI_STR_CONNECTING_TITLE, font_lg, MICHI_UI_TEXT_PRIMARY);

    const char *sub = (ssid != NULL && ssid[0] != '\0') ? ssid : MICHI_UI_STR_CONNECTING_WIFI;
    ui_draw_text(fb, fb_w, fb_h, y_origin, 100, 124, sub, font_sm, MICHI_UI_TEXT_SECONDARY);
}

void michi_ui_draw_screen_pairing(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin,
                                  const char *pin)
{
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);

    michi_ui_draw_header_landscape(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND, true, -50, false);
    michi_ui_draw_divider(fb, fb_w, fb_h, y_origin, false);

    if (pin != NULL && pin[0] != '\0') {
        /* Two-column layout */
        /* Left zone: x=16, w=125 */
        ui_draw_text(fb, fb_w, fb_h, y_origin, 16, 75, MICHI_UI_STR_PAIRING_TITLE, font_lg, MICHI_UI_TEXT_PRIMARY);

        michi_ui_rect_t r = { .x = 16, .y = 112, .w = 125, .h = 80 };
        michi_ui_draw_multiline(fb, fb_w, fb_h, y_origin, &r, MICHI_FONT_SM,
                                MICHI_UI_STR_PAIRING_PIN_HINT, MICHI_UI_ALIGN_LEFT, MICHI_UI_TEXT_SECONDARY);

        /* Right zone: PIN centered at cx=230, cy=115 in 41px PIN font */
        michi_ui_draw_pin_landscape(fb, fb_w, fb_h, y_origin, 230, 115, pin, MICHI_UI_ACCENT);

        /* 5 Activity dots at cx=230, y=175 */
        michi_ui_draw_activity_dots(fb, fb_w, fb_h, y_origin, 230, 175, 5, 12, MICHI_UI_ACCENT);
    } else {
        /* Waiting for PIN */
        michi_ui_draw_icon(fb, fb_w, fb_h, y_origin, 45, 95, MICHI_ICON_PAIR, 32, MICHI_UI_ACCENT);
        ui_draw_text(fb, fb_w, fb_h, y_origin, 100, 88, MICHI_UI_STR_PAIRING_TITLE, font_lg, MICHI_UI_TEXT_PRIMARY);
        ui_draw_text(fb, fb_w, fb_h, y_origin, 100, 124, MICHI_UI_STR_PAIRING_WAITING, font_sm, MICHI_UI_TEXT_SECONDARY);
    }
}

void michi_ui_draw_screen_session_pending(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin)
{
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);

    michi_ui_draw_header_landscape(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND, true, -50, true);
    michi_ui_draw_divider(fb, fb_w, fb_h, y_origin, false);

    michi_ui_draw_icon(fb, fb_w, fb_h, y_origin, 45, 95, MICHI_ICON_WAVE, 32, MICHI_UI_ACCENT);
    ui_draw_text(fb, fb_w, fb_h, y_origin, 100, 88, MICHI_UI_STR_SESSION_PREP, font_lg, MICHI_UI_TEXT_PRIMARY);
    ui_draw_text(fb, fb_w, fb_h, y_origin, 100, 124, MICHI_UI_STR_DEFAULT_SERVER, font_sm, MICHI_UI_TEXT_SECONDARY);
}

void michi_ui_draw_screen_buffering(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin,
                                    const michi_ui_screen_ctx_t *ctx)
{
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);
    const michi_ui_font_t *font_md = michi_ui_font_get(MICHI_FONT_MD);
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);

    michi_ui_draw_header_landscape(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND, true, -50, true);
    michi_ui_draw_divider(fb, fb_w, fb_h, y_origin, false);

    bool has_meta = (ctx != NULL && ctx->title != NULL && ctx->title[0] != '\0');

    if (has_meta) {
        char title_buf[128];
        const char *lines[4];
        bool truncated = false;
        strncpy(title_buf, ctx->title, sizeof(title_buf) - 1);
        title_buf[sizeof(title_buf) - 1] = '\0';

        int num_lines = ui_wrap_text_ex(font_lg, title_buf, 288, lines, 2, &truncated);
        int y = 62;
        for (int i = 0; i < num_lines; i++) {
            ui_draw_text(fb, fb_w, fb_h, y_origin, 16, y, lines[i], font_lg, MICHI_UI_TEXT_PRIMARY);
            y += michi_ui_font_line_height(font_lg);
        }

        if (ctx->artist != NULL && ctx->artist[0] != '\0') {
            ui_draw_text(fb, fb_w, fb_h, y_origin, 16, y + 4, ctx->artist, font_md, MICHI_UI_TEXT_SECONDARY);
        }

        /* Activity indication at y=152 */
        michi_ui_draw_activity_dots(fb, fb_w, fb_h, y_origin, 30, 158, 3, 8, MICHI_UI_INFO);
        ui_draw_text(fb, fb_w, fb_h, y_origin, 50, 152, MICHI_UI_STR_AUDIO_PREP, font_sm, MICHI_UI_INFO);
    } else {
        /* Centered buffering layout */
        michi_ui_draw_icon(fb, fb_w, fb_h, y_origin, (320 - 32) / 2, 65, MICHI_ICON_WAVE, 32, MICHI_UI_INFO);
        ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 110, MICHI_UI_STR_AUDIO_PREP, font_lg, MICHI_UI_TEXT_PRIMARY);
        ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 146, MICHI_UI_STR_DEFAULT_SERVER, font_sm, MICHI_UI_TEXT_SECONDARY);
    }

    char fmt[32];
    format_audio_spec(fmt, sizeof(fmt), ctx != NULL ? ctx->sample_rate : 48000, ctx != NULL ? ctx->bit_depth : 16);
    michi_ui_draw_playback_footer_landscape(fb, fb_w, fb_h, y_origin, ctx != NULL ? ctx->volume : 72, fmt);
}

void michi_ui_draw_screen_playing(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin,
                                  const michi_ui_screen_ctx_t *ctx)
{
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);
    const michi_ui_font_t *font_md = michi_ui_font_get(MICHI_FONT_MD);
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);

    michi_ui_draw_header_landscape(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND, true, -50, true);
    michi_ui_draw_divider(fb, fb_w, fb_h, y_origin, true);

    bool has_meta = (ctx != NULL && ctx->title != NULL && ctx->title[0] != '\0');

    if (has_meta) {
        char title_buf[128];
        char artist_buf[128];
        const char *lines[4];
        bool truncated = false;

        strncpy(title_buf, ctx->title, sizeof(title_buf) - 1);
        title_buf[sizeof(title_buf) - 1] = '\0';

        int num_lines = ui_wrap_text_ex(font_lg, title_buf, 288, lines, 2, &truncated);
        int y = 62;
        for (int i = 0; i < num_lines; i++) {
            ui_draw_text(fb, fb_w, fb_h, y_origin, 16, y, lines[i], font_lg, MICHI_UI_TEXT_PRIMARY);
            y += michi_ui_font_line_height(font_lg);
        }

        /* Artist */
        if (ctx->artist != NULL && ctx->artist[0] != '\0') {
            strncpy(artist_buf, ctx->artist, sizeof(artist_buf) - 1);
            artist_buf[sizeof(artist_buf) - 1] = '\0';
            int art_lines = ui_wrap_text_ex(font_md, artist_buf, 288, lines, 2, &truncated);
            int art_y = y + 6;
            for (int i = 0; i < art_lines; i++) {
                ui_draw_text(fb, fb_w, fb_h, y_origin, 16, art_y, lines[i], font_md, MICHI_UI_TEXT_SECONDARY);
                art_y += michi_ui_font_line_height(font_md);
            }
        }

        /* Source at x=16, y=172 */
        const char *src = (ctx->source != NULL && ctx->source[0] != '\0') ? ctx->source : MICHI_UI_STR_DEFAULT_SERVER;
        ui_draw_text(fb, fb_w, fb_h, y_origin, 16, 172, src, font_sm, MICHI_UI_TEXT_TERTIARY);
    } else {
        /* Fallback when no metadata is available */
        michi_ui_draw_icon(fb, fb_w, fb_h, y_origin, (320 - 32) / 2, 65, MICHI_ICON_PLAY, 32, MICHI_UI_ACCENT);
        ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 110, MICHI_UI_STR_PLAYING, font_lg, MICHI_UI_TEXT_PRIMARY);
        ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 146, MICHI_UI_STR_DEFAULT_SERVER, font_sm, MICHI_UI_TEXT_SECONDARY);
    }

    char fmt[32];
    format_audio_spec(fmt, sizeof(fmt), ctx != NULL ? ctx->sample_rate : 48000, ctx != NULL ? ctx->bit_depth : 16);
    michi_ui_draw_playback_footer_landscape(fb, fb_w, fb_h, y_origin, ctx != NULL ? ctx->volume : 72, fmt);
}

void michi_ui_draw_screen_paused(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin,
                                 const michi_ui_screen_ctx_t *ctx)
{
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);
    const michi_ui_font_t *font_md = michi_ui_font_get(MICHI_FONT_MD);
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);

    michi_ui_draw_header_landscape(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND, true, -50, true);
    michi_ui_draw_divider(fb, fb_w, fb_h, y_origin, false);

    bool has_meta = (ctx != NULL && ctx->title != NULL && ctx->title[0] != '\0');

    if (has_meta) {
        char title_buf[128];
        char artist_buf[128];
        const char *lines[4];
        bool truncated = false;

        strncpy(title_buf, ctx->title, sizeof(title_buf) - 1);
        title_buf[sizeof(title_buf) - 1] = '\0';

        int num_lines = ui_wrap_text_ex(font_lg, title_buf, 288, lines, 2, &truncated);
        int y = 62;
        for (int i = 0; i < num_lines; i++) {
            ui_draw_text(fb, fb_w, fb_h, y_origin, 16, y, lines[i], font_lg, MICHI_UI_TEXT_SECONDARY);
            y += michi_ui_font_line_height(font_lg);
        }

        if (ctx->artist != NULL && ctx->artist[0] != '\0') {
            strncpy(artist_buf, ctx->artist, sizeof(artist_buf) - 1);
            artist_buf[sizeof(artist_buf) - 1] = '\0';
            int art_lines = ui_wrap_text_ex(font_md, artist_buf, 288, lines, 2, &truncated);
            int art_y = y + 6;
            for (int i = 0; i < art_lines; i++) {
                ui_draw_text(fb, fb_w, fb_h, y_origin, 16, art_y, lines[i], font_md, MICHI_UI_TEXT_TERTIARY);
                art_y += michi_ui_font_line_height(font_md);
            }
        }

        /* "Pausa" indicator at x=16, y=172 in ACCENT_SOFT */
        ui_draw_text(fb, fb_w, fb_h, y_origin, 16, 172, MICHI_UI_STR_PAUSED, font_sm, MICHI_UI_ACCENT_SOFT);
    } else {
        michi_ui_draw_icon(fb, fb_w, fb_h, y_origin, (320 - 32) / 2, 65, MICHI_ICON_PAUSE, 32, MICHI_UI_ACCENT_SOFT);
        ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 110, MICHI_UI_STR_PAUSED, font_lg, MICHI_UI_TEXT_PRIMARY);
        ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 146, MICHI_UI_STR_DEFAULT_SERVER, font_sm, MICHI_UI_TEXT_SECONDARY);
    }

    char fmt[32];
    format_audio_spec(fmt, sizeof(fmt), ctx != NULL ? ctx->sample_rate : 48000, ctx != NULL ? ctx->bit_depth : 16);
    michi_ui_draw_playback_footer_landscape(fb, fb_w, fb_h, y_origin, ctx != NULL ? ctx->volume : 72, fmt);
}

void michi_ui_draw_screen_updating(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin,
                                   uint8_t pct)
{
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);

    michi_ui_draw_header_landscape(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND, true, -50, false);
    michi_ui_draw_divider(fb, fb_w, fb_h, y_origin, false);

    /* Update icon 32x32 at (45, 75) */
    michi_ui_draw_icon(fb, fb_w, fb_h, y_origin, 45, 75, MICHI_ICON_UPDATE, 32, MICHI_UI_ACCENT);

    /* Text block */
    ui_draw_text(fb, fb_w, fb_h, y_origin, 95, 72, MICHI_UI_STR_UPDATING_TITLE, font_lg, MICHI_UI_TEXT_PRIMARY);
    ui_draw_text(fb, fb_w, fb_h, y_origin, 95, 104, MICHI_UI_STR_UPDATING_HINT, font_sm, MICHI_UI_TEXT_SECONDARY);

    /* Progress bar at x=40, y=150, w=240, h=4 */
    michi_ui_draw_progress_landscape(fb, fb_w, fb_h, y_origin, 40, 150, 240, 4, pct);

    /* Percentage text at center */
    if (pct > 0) {
        char pct_str[16];
        snprintf(pct_str, sizeof(pct_str), "%u%%", (unsigned)pct);
        ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 165, pct_str, font_sm, MICHI_UI_TEXT_TERTIARY);
    }
}

void michi_ui_draw_screen_recoverable_error(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin,
                                            uint32_t error_code)
{
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);

    michi_ui_draw_header_landscape(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND, false, 0, false);
    michi_ui_draw_divider(fb, fb_w, fb_h, y_origin, false);

    /* Warning icon 32x32 at (45, 95) */
    michi_ui_draw_icon(fb, fb_w, fb_h, y_origin, 45, 95, MICHI_ICON_WARNING, 32, MICHI_UI_WARNING);

    /* Differentiate Network vs Audio recovery if error code indicates audio */
    bool is_audio = (error_code >= 0x3000 && error_code <= 0x3FFF);
    const char *title = is_audio ? MICHI_UI_STR_RECOVERING_AUDIO : MICHI_UI_STR_RECOVERING_TITLE;
    const char *hint = is_audio ? MICHI_UI_STR_RECOVERING_AUDIO_HINT : MICHI_UI_STR_RECOVERING_HINT;

    ui_draw_text(fb, fb_w, fb_h, y_origin, 95, 88, title, font_lg, MICHI_UI_TEXT_PRIMARY);
    ui_draw_text(fb, fb_w, fb_h, y_origin, 95, 124, hint, font_sm, MICHI_UI_TEXT_SECONDARY);
}

void michi_ui_draw_screen_fatal_error(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin,
                                      uint32_t error_code)
{
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);
    const michi_ui_font_t *font_xs = michi_ui_font_get(MICHI_FONT_XS);

    michi_ui_draw_header_landscape(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND, false, 0, false);
    michi_ui_draw_divider(fb, fb_w, fb_h, y_origin, false);

    /* Error icon 32x32 at (45, 90) */
    michi_ui_draw_icon(fb, fb_w, fb_h, y_origin, 45, 90, MICHI_ICON_ERROR, 32, MICHI_UI_ERROR);

    ui_draw_text(fb, fb_w, fb_h, y_origin, 95, 78, MICHI_UI_STR_FATAL_TITLE, font_lg, MICHI_UI_TEXT_PRIMARY);
    ui_draw_text(fb, fb_w, fb_h, y_origin, 95, 114, MICHI_UI_STR_FATAL_HINT, font_sm, MICHI_UI_TEXT_SECONDARY);

    if (error_code != 0) {
        char code_buf[32];
        snprintf(code_buf, sizeof(code_buf), "Código E%" PRIu32, error_code & 0xFFFF);
        ui_draw_text(fb, fb_w, fb_h, y_origin, 95, 142, code_buf, font_xs, MICHI_UI_TEXT_TERTIARY);
    }
}

void michi_ui_draw_screen_diagnostics(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin,
                                      const michi_ui_screen_ctx_t *ctx)
{
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);

    /* Header at y=14 */
    ui_draw_text(fb, fb_w, fb_h, y_origin, 16, 14, MICHI_UI_STR_DIAGNOSTICS_TITLE, font_sm, MICHI_UI_TEXT_PRIMARY);
    michi_ui_draw_divider(fb, fb_w, fb_h, y_origin, false);

    int y = 52;
    int row_h = 22;

    typedef struct diag_row {
        const char *label;
        char val[48];
        uint16_t val_color;
    } diag_row_t;

    diag_row_t rows[8];
    int r_count = 0;

    /* Row 0: Wi-Fi */
    rows[r_count].label = "Wi-Fi";
    if (ctx != NULL && ctx->wifi_connected) {
        if (ctx->wifi_rssi != 0) {
            snprintf(rows[r_count].val, sizeof(rows[r_count].val), "Connected (%d dBm)", (int)ctx->wifi_rssi);
        } else {
            snprintf(rows[r_count].val, sizeof(rows[r_count].val), "Connected");
        }
        rows[r_count].val_color = MICHI_UI_SUCCESS;
    } else {
        snprintf(rows[r_count].val, sizeof(rows[r_count].val), "Disconnected");
        rows[r_count].val_color = MICHI_UI_MUTED;
    }
    r_count++;

    /* Row 1: Server */
    rows[r_count].label = "Server";
    if (ctx != NULL && ctx->server_connected) {
        snprintf(rows[r_count].val, sizeof(rows[r_count].val), "Connected");
        rows[r_count].val_color = MICHI_UI_SUCCESS;
    } else {
        snprintf(rows[r_count].val, sizeof(rows[r_count].val), "Disconnected");
        rows[r_count].val_color = MICHI_UI_MUTED;
    }
    r_count++;

    /* Row 2: DAC (Truthful) */
    rows[r_count].label = "DAC";
    if (ctx != NULL && ctx->dac_detected && ctx->dac_model != NULL && ctx->dac_model[0] != '\0') {
        snprintf(rows[r_count].val, sizeof(rows[r_count].val), "%s", ctx->dac_model);
        rows[r_count].val_color = MICHI_UI_SUCCESS;
    } else if (ctx != NULL && !ctx->dac_detected) {
        snprintf(rows[r_count].val, sizeof(rows[r_count].val), "%s", MICHI_UI_STR_DAC_NONE);
        rows[r_count].val_color = MICHI_UI_WARNING;
    } else {
        snprintf(rows[r_count].val, sizeof(rows[r_count].val), "%s", MICHI_UI_STR_DAC_UNKNOWN);
        rows[r_count].val_color = MICHI_UI_MUTED;
    }
    r_count++;

    /* Row 3: Audio */
    rows[r_count].label = "Audio";
    snprintf(rows[r_count].val, sizeof(rows[r_count].val), "%" PRIu32 "k / %u-bit",
             (ctx != NULL && ctx->sample_rate != 0) ? (ctx->sample_rate / 1000u) : 48u,
             (ctx != NULL && ctx->bit_depth != 0) ? (unsigned)ctx->bit_depth : 16u);
    rows[r_count].val_color = MICHI_UI_TEXT_PRIMARY;
    r_count++;

    /* Row 4: Volume */
    rows[r_count].label = "Volume";
    snprintf(rows[r_count].val, sizeof(rows[r_count].val), "%u", (ctx != NULL) ? (unsigned)ctx->volume : 72u);
    rows[r_count].val_color = MICHI_UI_TEXT_PRIMARY;
    r_count++;

    /* Row 5: PSRAM */
    rows[r_count].label = "PSRAM";
    uint32_t psram_mb = (ctx != NULL && ctx->psram_bytes != 0) ? (ctx->psram_bytes / (1024u * 1024u)) : 8u;
    snprintf(rows[r_count].val, sizeof(rows[r_count].val), "%" PRIu32 " MB", psram_mb);
    rows[r_count].val_color = MICHI_UI_TEXT_PRIMARY;
    r_count++;

    /* Row 6: Firmware */
    rows[r_count].label = "Firmware";
    const char *fw = (ctx != NULL && ctx->fw_version != NULL) ? ctx->fw_version : MICHI_FW_VERSION_STR;
    snprintf(rows[r_count].val, sizeof(rows[r_count].val), "%s", fw);
    rows[r_count].val_color = MICHI_UI_TEXT_SECONDARY;
    r_count++;

    for (int i = 0; i < r_count; i++) {
        ui_draw_text(fb, fb_w, fb_h, y_origin, 16, y, rows[i].label, font_sm, MICHI_UI_TEXT_TERTIARY);

        int val_w = ui_text_measure(font_sm, rows[i].val);
        int val_x = 304 - val_w;
        if (val_x < 120) {
            val_x = 120;
        }
        ui_draw_text(fb, fb_w, fb_h, y_origin, val_x, y, rows[i].val, font_sm, rows[i].val_color);
        y += row_h;
    }
}

void michi_ui_render_screen(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                            uint16_t y_origin, const michi_ui_screen_ctx_t *ctx)
{
    if (fb == NULL || fb_w == 0 || fb_h == 0) {
        return;
    }

    /* Fill band with the real background color #080A0F */
    ui_clear_band(fb, fb_w, fb_h, MICHI_UI_BG);

    if (ctx == NULL) {
        return;
    }

    /* Diagnostics priority view */
    if (ctx->show_diagnostics) {
        michi_ui_draw_screen_diagnostics(fb, fb_w, fb_h, y_origin, ctx);
        return;
    }

    /* Temporary volume overlay */
    if (ctx->show_volume_overlay) {
        michi_ui_draw_volume_overlay(fb, fb_w, fb_h, y_origin, ctx->volume);
        return;
    }

    switch (ctx->state) {
    case MICHI_STATE_BOOTING:
    case MICHI_STATE_SELF_TEST:
        michi_ui_draw_screen_boot(fb, fb_w, fb_h, y_origin);
        break;
    case MICHI_STATE_UNPROVISIONED:
        michi_ui_draw_screen_unprovisioned(fb, fb_w, fb_h, y_origin);
        break;
    case MICHI_STATE_IDLE:
        michi_ui_draw_screen_ready(fb, fb_w, fb_h, y_origin, ctx);
        break;
    case MICHI_STATE_PROVISIONING:
        michi_ui_draw_screen_provisioning(fb, fb_w, fb_h, y_origin);
        break;
    case MICHI_STATE_WIFI_CONNECTING:
        michi_ui_draw_screen_connecting(fb, fb_w, fb_h, y_origin, ctx->wifi_ssid);
        break;
    case MICHI_STATE_PAIRING:
        michi_ui_draw_screen_pairing(fb, fb_w, fb_h, y_origin, ctx->pairing_pin);
        break;
    case MICHI_STATE_SESSION_PENDING:
        michi_ui_draw_screen_session_pending(fb, fb_w, fb_h, y_origin);
        break;
    case MICHI_STATE_BUFFERING:
        michi_ui_draw_screen_buffering(fb, fb_w, fb_h, y_origin, ctx);
        break;
    case MICHI_STATE_PLAYING:
        michi_ui_draw_screen_playing(fb, fb_w, fb_h, y_origin, ctx);
        break;
    case MICHI_STATE_PAUSED:
        michi_ui_draw_screen_paused(fb, fb_w, fb_h, y_origin, ctx);
        break;
    case MICHI_STATE_UPDATING:
        michi_ui_draw_screen_updating(fb, fb_w, fb_h, y_origin, ctx->update_pct);
        break;
    case MICHI_STATE_RECOVERABLE_ERROR:
        michi_ui_draw_screen_recoverable_error(fb, fb_w, fb_h, y_origin, ctx->last_error);
        break;
    case MICHI_STATE_FATAL_ERROR:
        michi_ui_draw_screen_fatal_error(fb, fb_w, fb_h, y_origin, ctx->last_error);
        break;
    default:
        michi_ui_draw_screen_boot(fb, fb_w, fb_h, y_origin);
        break;
    }
}
