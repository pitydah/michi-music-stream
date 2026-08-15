#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "michi_ui.h"

/* Max string length for line copies */
#define MICHI_UI_LINE_MAX 128

/* Format helper for UX error codes (Section 28) */
static void format_error_code(uint32_t err_code, char *buf, size_t buf_len)
{
    if (err_code == 0) {
        snprintf(buf, buf_len, "%sE100", MICHI_UI_STR_ERROR_CODE_PREFIX);
        return;
    }
    /* WiFi / Network range: 0x3000..0x5FFF */
    if (err_code >= 0x3000 && err_code <= 0x5FFF) {
        snprintf(buf, buf_len, "%sE101", MICHI_UI_STR_ERROR_CODE_PREFIX);
        return;
    }
    /* Audio / Stream / State range */
    if (err_code == 0x103 /* ESP_ERR_INVALID_STATE */ ||
        err_code == 0x107 /* ESP_ERR_TIMEOUT */) {
        snprintf(buf, buf_len, "%sE102", MICHI_UI_STR_ERROR_CODE_PREFIX);
        return;
    }
    /* Storage / Flash / NVS: 0x1100..0x11FF */
    if (err_code >= 0x1100 && err_code <= 0x11FF) {
        snprintf(buf, buf_len, "%sE103", MICHI_UI_STR_ERROR_CODE_PREFIX);
        return;
    }
    /* Display / Allocation */
    if (err_code == 0x101 /* ESP_ERR_NO_MEM */) {
        snprintf(buf, buf_len, "%sE104", MICHI_UI_STR_ERROR_CODE_PREFIX);
        return;
    }
    /* General internal error */
    snprintf(buf, buf_len, "%sE%03u", MICHI_UI_STR_ERROR_CODE_PREFIX,
             (unsigned)(err_code % 1000u));
}

void michi_ui_draw_screen_boot(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                               uint16_t y_origin)
{
    /* Cat icon centered at (120, 95), size 48x48 */
    ui_draw_icon(fb, fb_w, fb_h, y_origin, 120 - 24, 95, MICHI_UI_ICON_CAT,
                 MICHI_UI_ICON_SIZE_48, MICHI_UI_TEXT_PRIMARY);

    /* "michi" centered at y=152 in LG font (baseline ~168) */
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 152,
                          MICHI_UI_STR_BRAND_LOWER,
                          michi_ui_font_get(MICHI_FONT_LG),
                          MICHI_UI_TEXT_PRIMARY);

    /* "iniciando" centered at y=195 in SM font (baseline ~205) */
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 195,
                          MICHI_UI_STR_STARTING,
                          michi_ui_font_get(MICHI_FONT_SM),
                          MICHI_UI_TEXT_SECONDARY);

    /* 3 activity dots centered at y=235 */
    michi_ui_draw_activity_dots(fb, fb_w, fb_h, y_origin, 120, 235, 3, 10,
                                MICHI_UI_MUTED);
}

void michi_ui_draw_screen_ready(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                uint16_t y_origin, const michi_ui_screen_ctx_t *ctx)
{
    (void)ctx;
    /* Header: "Michi" left, green status dot right */
    michi_ui_draw_header_bar(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND,
                             0, false, 0);
    michi_ui_draw_status_dot(fb, fb_w, fb_h, y_origin, 220, 19,
                             MICHI_UI_SUCCESS);

    /* Cat icon centered at (120, 75), size 48x48 */
    ui_draw_icon(fb, fb_w, fb_h, y_origin, 120 - 24, 75, MICHI_UI_ICON_CAT,
                 MICHI_UI_ICON_SIZE_48, MICHI_UI_TEXT_PRIMARY);

    /* "Listo" in LG */
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 150,
                          MICHI_UI_STR_READY,
                          michi_ui_font_get(MICHI_FONT_LG),
                          MICHI_UI_TEXT_PRIMARY);

    /* "Esperando reproducción" in SM */
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 185,
                          MICHI_UI_STR_WAITING_PLAYBACK,
                          michi_ui_font_get(MICHI_FONT_SM),
                          MICHI_UI_TEXT_SECONDARY);
}

void michi_ui_draw_screen_unprovisioned(uint16_t *fb, uint16_t fb_w,
                                       uint16_t fb_h, uint16_t y_origin)
{
    const michi_ui_rect_t hint_rect = { 20, 165, 200, 45 };

    michi_ui_draw_header_bar(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND,
                             0, false, 0);

    /* Cat icon centered at (120, 65), size 48x48 */
    ui_draw_icon(fb, fb_w, fb_h, y_origin, 120 - 24, 65, MICHI_UI_ICON_CAT,
                 MICHI_UI_ICON_SIZE_48, MICHI_UI_TEXT_PRIMARY);

    /* "Configurar Michi" in LG */
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 125,
                          MICHI_UI_STR_SETUP_TITLE,
                          michi_ui_font_get(MICHI_FONT_LG),
                          MICHI_UI_TEXT_PRIMARY);

    /* Hint text centered in multiline */
    (void)michi_ui_draw_multiline(fb, fb_w, fb_h, y_origin, &hint_rect,
                                  MICHI_FONT_SM, MICHI_UI_STR_SETUP_HINT,
                                  MICHI_UI_ALIGN_CENTER,
                                  MICHI_UI_TEXT_SECONDARY);

    /* Physical button icon centered at (120, 220), size 32x32 */
    ui_draw_icon(fb, fb_w, fb_h, y_origin, 120 - 16, 220, MICHI_UI_ICON_BUTTON,
                 MICHI_UI_ICON_SIZE_32, MICHI_UI_ACCENT);
}

void michi_ui_draw_screen_provisioning(uint16_t *fb, uint16_t fb_w,
                                      uint16_t fb_h, uint16_t y_origin)
{
    const michi_ui_rect_t hint_rect = { 20, 160, 200, 50 };

    michi_ui_draw_header_bar(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND,
                             0, false, 0);

    /* Wi-Fi icon centered at (120, 75), size 32x32 */
    ui_draw_icon(fb, fb_w, fb_h, y_origin, 120 - 16, 75, MICHI_UI_ICON_WIFI,
                 MICHI_UI_ICON_SIZE_32, MICHI_UI_ACCENT);

    /* "Configurando red" */
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 125,
                          MICHI_UI_STR_PROV_TITLE,
                          michi_ui_font_get(MICHI_FONT_LG),
                          MICHI_UI_TEXT_PRIMARY);

    (void)michi_ui_draw_multiline(fb, fb_w, fb_h, y_origin, &hint_rect,
                                  MICHI_FONT_SM, MICHI_UI_STR_PROV_HINT,
                                  MICHI_UI_ALIGN_CENTER,
                                  MICHI_UI_TEXT_SECONDARY);
}

void michi_ui_draw_screen_connecting(uint16_t *fb, uint16_t fb_w,
                                     uint16_t fb_h, uint16_t y_origin,
                                     const char *ssid)
{
    michi_ui_draw_header_bar(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND,
                             0, false, 0);

    /* Wi-Fi icon centered at (120, 75), size 32x32 */
    ui_draw_icon(fb, fb_w, fb_h, y_origin, 120 - 16, 75, MICHI_UI_ICON_WIFI,
                 MICHI_UI_ICON_SIZE_32, MICHI_UI_INFO);

    /* "Conectando" */
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 125,
                          MICHI_UI_STR_CONNECTING_TITLE,
                          michi_ui_font_get(MICHI_FONT_LG),
                          MICHI_UI_TEXT_PRIMARY);

    /* Subtitle: SSID or "A la red Wi-Fi" */
    const char *sub = (ssid != NULL && ssid[0] != '\0') ?
                      ssid : MICHI_UI_STR_CONNECTING_WIFI;
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 160, sub,
                          michi_ui_font_get(MICHI_FONT_SM),
                          MICHI_UI_TEXT_SECONDARY);
}

void michi_ui_draw_screen_pairing(uint16_t *fb, uint16_t fb_w,
                                  uint16_t fb_h, uint16_t y_origin,
                                  const char *pin)
{
    michi_ui_draw_header_bar(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND,
                             0, false, 0);

    if (pin == NULL || pin[0] == '\0') {
        /* Pairing without active PIN yet */
        ui_draw_icon(fb, fb_w, fb_h, y_origin, 120 - 16, 75, MICHI_UI_ICON_PAIR,
                     MICHI_UI_ICON_SIZE_32, MICHI_UI_ACCENT);

        ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 125,
                              MICHI_UI_STR_PAIRING_TITLE,
                              michi_ui_font_get(MICHI_FONT_LG),
                              MICHI_UI_TEXT_PRIMARY);

        ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 160,
                              MICHI_UI_STR_PAIRING_WAITING,
                              michi_ui_font_get(MICHI_FONT_SM),
                              MICHI_UI_TEXT_SECONDARY);
        return;
    }

    /* Pairing with PIN (P0 screen) */
    /* "Vincular" headline */
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 70,
                          MICHI_UI_STR_PAIRING_TITLE,
                          michi_ui_font_get(MICHI_FONT_MD),
                          MICHI_UI_TEXT_PRIMARY);

    /* PIN rendered centered at y=145 in PIN font */
    michi_ui_draw_pin(fb, fb_w, fb_h, y_origin, 145, pin, MICHI_UI_ACCENT);

    /* Instruction hint */
    const michi_ui_rect_t inst_rect = { 15, 195, 210, 40 };
    (void)michi_ui_draw_multiline(fb, fb_w, fb_h, y_origin, &inst_rect,
                                  MICHI_FONT_SM, MICHI_UI_STR_PAIRING_PIN_HINT,
                                  MICHI_UI_ALIGN_CENTER,
                                  MICHI_UI_TEXT_SECONDARY);

    /* 5 activity dots at y=268 */
    michi_ui_draw_activity_dots(fb, fb_w, fb_h, y_origin, 120, 268, 5, 12,
                                MICHI_UI_MUTED);
}

void michi_ui_draw_screen_session_pending(uint16_t *fb, uint16_t fb_w,
                                         uint16_t fb_h, uint16_t y_origin)
{
    michi_ui_draw_header_bar(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND,
                             0, false, 0);

    /* Audio wave icon centered at (120, 75), size 32x32 */
    ui_draw_icon(fb, fb_w, fb_h, y_origin, 120 - 16, 75, MICHI_UI_ICON_WAVE,
                 MICHI_UI_ICON_SIZE_32, MICHI_UI_ACCENT);

    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 125,
                          MICHI_UI_STR_SESSION_PREP,
                          michi_ui_font_get(MICHI_FONT_LG),
                          MICHI_UI_TEXT_PRIMARY);

    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 160,
                          MICHI_UI_STR_DEFAULT_SERVER,
                          michi_ui_font_get(MICHI_FONT_SM),
                          MICHI_UI_TEXT_SECONDARY);
}

void michi_ui_draw_screen_buffering(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                    uint16_t y_origin, const michi_ui_screen_ctx_t *ctx)
{
    bool has_meta = (ctx != NULL && ctx->title != NULL && ctx->title[0] != '\0');

    if (has_meta) {
        /* Render playing screen with audio prep indication */
        michi_ui_draw_screen_playing(fb, fb_w, fb_h, y_origin, ctx);
        return;
    }

    michi_ui_draw_header_bar(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND,
                             0, false, 0);

    ui_draw_icon(fb, fb_w, fb_h, y_origin, 120 - 16, 75, MICHI_UI_ICON_WAVE,
                 MICHI_UI_ICON_SIZE_32, MICHI_UI_ACCENT);

    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 125,
                          MICHI_UI_STR_AUDIO_PREP,
                          michi_ui_font_get(MICHI_FONT_LG),
                          MICHI_UI_TEXT_PRIMARY);

    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 160,
                          MICHI_UI_STR_DEFAULT_SERVER,
                          michi_ui_font_get(MICHI_FONT_SM),
                          MICHI_UI_TEXT_SECONDARY);
}

/* Helper to format the audio stream format string (e.g. "48/16" or "48 kHz") */
static void format_audio_stream(char *buf, size_t buf_len,
                                uint32_t sample_rate, uint8_t bit_depth)
{
    uint32_t khz = sample_rate > 0 ? sample_rate / 1000u : 48u;
    uint8_t bits = bit_depth > 0 ? bit_depth : 16u;
    snprintf(buf, buf_len, "%" PRIu32 "/%u", khz, (unsigned)bits);
}

void michi_ui_draw_screen_playing(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                  uint16_t y_origin, const michi_ui_screen_ctx_t *ctx)
{
    char format_buf[16];
    format_audio_stream(format_buf, sizeof(format_buf),
                        ctx != NULL ? ctx->sample_rate : 48000u,
                        ctx != NULL ? ctx->bit_depth : 16u);

    /* Header: "Michi" left, Wave icon right */
    michi_ui_draw_header_bar(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND,
                             MICHI_UI_ICON_WAVE, true, MICHI_UI_SUCCESS);

    bool has_meta = (ctx != NULL && ctx->title != NULL && ctx->title[0] != '\0');

    if (!has_meta) {
        /* Fallback screen when no metadata is available */
        ui_draw_icon(fb, fb_w, fb_h, y_origin, 120 - 16, 70, MICHI_UI_ICON_PLAY,
                     MICHI_UI_ICON_SIZE_32, MICHI_UI_ACCENT);

        ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 125,
                              MICHI_UI_STR_PLAYING,
                              michi_ui_font_get(MICHI_FONT_LG),
                              MICHI_UI_TEXT_PRIMARY);

        ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 160,
                              MICHI_UI_STR_DEFAULT_SERVER,
                              michi_ui_font_get(MICHI_FONT_SM),
                              MICHI_UI_TEXT_SECONDARY);

        michi_ui_draw_playback_footer(fb, fb_w, fb_h, y_origin,
                                     ctx != NULL ? ctx->volume : 50,
                                     format_buf);
        return;
    }

    /* Playing screen with metadata (P0 layout) */
    /* Central audio wave glyph at y=52 */
    ui_draw_icon(fb, fb_w, fb_h, y_origin, 120 - 10, 52, MICHI_UI_ICON_WAVE,
                 MICHI_UI_ICON_SIZE_20, MICHI_UI_ACCENT);

    /* Title: x=16, y=98, w=208, max 3 lines in LG font */
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);
    const int title_line_pitch = 23;
    char title_buf[MICHI_UI_LINE_MAX];
    const char *title_lines[4];
    int title_y = 98;
    int n_title = 0;

    strncpy(title_buf, ctx->title, sizeof(title_buf) - 1);
    title_buf[sizeof(title_buf) - 1] = '\0';
    n_title = ui_wrap_text(font_lg, title_buf, 208, title_lines, 3);
    if (n_title > 0) {
        for (int i = 0; i < n_title; i++) {
            char line_copy[MICHI_UI_LINE_MAX];
            strncpy(line_copy, title_lines[i], sizeof(line_copy) - 1);
            line_copy[sizeof(line_copy) - 1] = '\0';
            if (i == 2 && strlen(ctx->title) > strlen(title_buf)) {
                (void)ui_ellipsize(font_lg, line_copy, 208);
            }
            ui_draw_text(fb, fb_w, fb_h, y_origin, 16,
                         title_y + i * title_line_pitch,
                         line_copy, font_lg, MICHI_UI_TEXT_PRIMARY);
        }
    }

    /* Artist: y = title_bottom + 8, max 2 lines in MD font */
    int title_bottom = title_y + (n_title > 0 ? (n_title - 1) * title_line_pitch + 20 : 20);
    int artist_y = title_bottom + 8;
    if (ctx->artist != NULL && ctx->artist[0] != '\0') {
        const michi_ui_font_t *font_md = michi_ui_font_get(MICHI_FONT_MD);
        const int artist_line_pitch = 16;
        char artist_buf[MICHI_UI_LINE_MAX];
        const char *artist_lines[3];
        strncpy(artist_buf, ctx->artist, sizeof(artist_buf) - 1);
        artist_buf[sizeof(artist_buf) - 1] = '\0';
        int n_artist = ui_wrap_text(font_md, artist_buf, 208, artist_lines, 2);
        for (int i = 0; i < n_artist; i++) {
            char line_copy[MICHI_UI_LINE_MAX];
            strncpy(line_copy, artist_lines[i], sizeof(line_copy) - 1);
            line_copy[sizeof(line_copy) - 1] = '\0';
            if (i == 1 && strlen(ctx->artist) > strlen(artist_buf)) {
                (void)ui_ellipsize(font_md, line_copy, 208);
            }
            ui_draw_text(fb, fb_w, fb_h, y_origin, 16,
                         artist_y + i * artist_line_pitch,
                         line_copy, font_md, MICHI_UI_TEXT_SECONDARY);
        }
    }

    /* Source label at y ≈ 238 */
    const char *src = (ctx->source != NULL && ctx->source[0] != '\0') ?
                      ctx->source : MICHI_UI_STR_DEFAULT_SERVER;
    char src_buf[MICHI_UI_LINE_MAX];
    strncpy(src_buf, src, sizeof(src_buf) - 1);
    src_buf[sizeof(src_buf) - 1] = '\0';
    (void)ui_ellipsize(michi_ui_font_get(MICHI_FONT_SM), src_buf, 208);
    ui_draw_text(fb, fb_w, fb_h, y_origin, 16, 238, src_buf,
                 michi_ui_font_get(MICHI_FONT_SM), MICHI_UI_TEXT_TERTIARY);

    /* Footer: volume + audio format at y=286 */
    michi_ui_draw_playback_footer(fb, fb_w, fb_h, y_origin,
                                 ctx->volume, format_buf);
}

void michi_ui_draw_screen_paused(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                 uint16_t y_origin, const michi_ui_screen_ctx_t *ctx)
{
    char format_buf[16];
    format_audio_stream(format_buf, sizeof(format_buf),
                        ctx != NULL ? ctx->sample_rate : 48000u,
                        ctx != NULL ? ctx->bit_depth : 16u);

    /* Header: "Michi" left, Pause icon right */
    michi_ui_draw_header_bar(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND,
                             MICHI_UI_ICON_PAUSE, true, MICHI_UI_MUTED);

    bool has_meta = (ctx != NULL && ctx->title != NULL && ctx->title[0] != '\0');

    if (!has_meta) {
        ui_draw_icon(fb, fb_w, fb_h, y_origin, 120 - 16, 70, MICHI_UI_ICON_PAUSE,
                     MICHI_UI_ICON_SIZE_32, MICHI_UI_ACCENT);

        ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 125,
                              MICHI_UI_STR_PAUSED,
                              michi_ui_font_get(MICHI_FONT_LG),
                              MICHI_UI_TEXT_PRIMARY);

        ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 160,
                              MICHI_UI_STR_DEFAULT_SERVER,
                              michi_ui_font_get(MICHI_FONT_SM),
                              MICHI_UI_TEXT_SECONDARY);

        michi_ui_draw_playback_footer(fb, fb_w, fb_h, y_origin,
                                     ctx != NULL ? ctx->volume : 50,
                                     format_buf);
        return;
    }

    /* Pause glyph at y=52 */
    ui_draw_icon(fb, fb_w, fb_h, y_origin, 120 - 10, 52, MICHI_UI_ICON_PAUSE,
                 MICHI_UI_ICON_SIZE_20, MICHI_UI_ACCENT);

    /* Title: LG */
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);
    const int title_line_pitch = 23;
    char title_buf[MICHI_UI_LINE_MAX];
    const char *title_lines[4];
    int title_y = 98;
    int n_title = 0;

    strncpy(title_buf, ctx->title, sizeof(title_buf) - 1);
    title_buf[sizeof(title_buf) - 1] = '\0';
    n_title = ui_wrap_text(font_lg, title_buf, 208, title_lines, 3);
    if (n_title > 0) {
        for (int i = 0; i < n_title; i++) {
            char line_copy[MICHI_UI_LINE_MAX];
            strncpy(line_copy, title_lines[i], sizeof(line_copy) - 1);
            line_copy[sizeof(line_copy) - 1] = '\0';
            if (i == 2 && strlen(ctx->title) > strlen(title_buf)) {
                (void)ui_ellipsize(font_lg, line_copy, 208);
            }
            ui_draw_text(fb, fb_w, fb_h, y_origin, 16,
                         title_y + i * title_line_pitch,
                         line_copy, font_lg, MICHI_UI_TEXT_PRIMARY);
        }
    }

    /* Artist */
    int title_bottom = title_y + (n_title > 0 ? (n_title - 1) * title_line_pitch + 20 : 20);
    int artist_y = title_bottom + 8;
    if (ctx->artist != NULL && ctx->artist[0] != '\0') {
        const michi_ui_font_t *font_md = michi_ui_font_get(MICHI_FONT_MD);
        const int artist_line_pitch = 16;
        char artist_buf[MICHI_UI_LINE_MAX];
        const char *artist_lines[3];
        strncpy(artist_buf, ctx->artist, sizeof(artist_buf) - 1);
        artist_buf[sizeof(artist_buf) - 1] = '\0';
        int n_artist = ui_wrap_text(font_md, artist_buf, 208, artist_lines, 2);
        for (int i = 0; i < n_artist; i++) {
            char line_copy[MICHI_UI_LINE_MAX];
            strncpy(line_copy, artist_lines[i], sizeof(line_copy) - 1);
            line_copy[sizeof(line_copy) - 1] = '\0';
            if (i == 1 && strlen(ctx->artist) > strlen(artist_buf)) {
                (void)ui_ellipsize(font_md, line_copy, 208);
            }
            ui_draw_text(fb, fb_w, fb_h, y_origin, 16,
                         artist_y + i * artist_line_pitch,
                         line_copy, font_md, MICHI_UI_TEXT_SECONDARY);
        }
    }

    /* Status: "Pausa" in accent color */
    ui_draw_text(fb, fb_w, fb_h, y_origin, 16, 238, MICHI_UI_STR_PAUSED,
                 michi_ui_font_get(MICHI_FONT_SM), MICHI_UI_ACCENT);

    /* Footer: Volume + Format */
    michi_ui_draw_playback_footer(fb, fb_w, fb_h, y_origin,
                                 ctx->volume, format_buf);
}

void michi_ui_draw_screen_updating(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                  uint16_t y_origin, uint8_t pct)
{
    michi_ui_draw_header_bar(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND,
                             0, false, 0);

    /* Download/update icon centered at (120, 75), size 32x32 */
    ui_draw_icon(fb, fb_w, fb_h, y_origin, 120 - 16, 75, MICHI_UI_ICON_UPDATE,
                 MICHI_UI_ICON_SIZE_32, MICHI_UI_ACCENT);

    /* "Actualizando" */
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 125,
                          MICHI_UI_STR_UPDATING_TITLE,
                          michi_ui_font_get(MICHI_FONT_LG),
                          MICHI_UI_TEXT_PRIMARY);

    /* "No desconectes Michi" */
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 160,
                          MICHI_UI_STR_UPDATING_HINT,
                          michi_ui_font_get(MICHI_FONT_SM),
                          MICHI_UI_TEXT_SECONDARY);

    /* Optional progress bar */
    if (pct > 0) {
        michi_ui_draw_progress(fb, fb_w, fb_h, y_origin, 30, 200, 180, 8, pct);
    }
}

void michi_ui_draw_screen_recoverable_error(uint16_t *fb, uint16_t fb_w,
                                           uint16_t fb_h, uint16_t y_origin,
                                           uint32_t error_code)
{
    (void)error_code;
    const michi_ui_rect_t hint_rect = { 20, 160, 200, 40 };

    michi_ui_draw_header_bar(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND,
                             0, false, 0);

    /* Warning icon centered at (120, 75), size 32x32 */
    ui_draw_icon(fb, fb_w, fb_h, y_origin, 120 - 16, 75, MICHI_UI_ICON_WARNING,
                 MICHI_UI_ICON_SIZE_32, MICHI_UI_WARNING);

    /* "Reconectando" */
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 125,
                          MICHI_UI_STR_RECOVERING_TITLE,
                          michi_ui_font_get(MICHI_FONT_LG),
                          MICHI_UI_TEXT_PRIMARY);

    /* Hint multiline */
    (void)michi_ui_draw_multiline(fb, fb_w, fb_h, y_origin, &hint_rect,
                                  MICHI_FONT_SM, MICHI_UI_STR_RECOVERING_HINT,
                                  MICHI_UI_ALIGN_CENTER,
                                  MICHI_UI_TEXT_SECONDARY);
}

void michi_ui_draw_screen_fatal_error(uint16_t *fb, uint16_t fb_w,
                                     uint16_t fb_h, uint16_t y_origin,
                                     uint32_t error_code)
{
    char err_buf[24];
    format_error_code(error_code, err_buf, sizeof(err_buf));

    michi_ui_draw_header_bar(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_BRAND,
                             0, false, 0);

    /* Error icon centered at (120, 75), size 32x32 */
    ui_draw_icon(fb, fb_w, fb_h, y_origin, 120 - 16, 75, MICHI_UI_ICON_ERROR,
                 MICHI_UI_ICON_SIZE_32, MICHI_UI_ERROR);

    /* "Algo falló" in LG */
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 125,
                          MICHI_UI_STR_FATAL_TITLE,
                          michi_ui_font_get(MICHI_FONT_LG),
                          MICHI_UI_TEXT_PRIMARY);

    /* "Reinicia Michi Stream" in SM */
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 160,
                          MICHI_UI_STR_FATAL_HINT,
                          michi_ui_font_get(MICHI_FONT_SM),
                          MICHI_UI_TEXT_SECONDARY);

    /* Short UX code e.g. "Código: E104" */
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 210, err_buf,
                          michi_ui_font_get(MICHI_FONT_SM),
                          MICHI_UI_TEXT_TERTIARY);
}

static void draw_diag_row(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                          uint16_t y_origin, int y, const char *label,
                          const char *value, uint16_t val_color)
{
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);
    ui_draw_text(fb, fb_w, fb_h, y_origin, 16, y, label,
                 font_sm, MICHI_UI_TEXT_SECONDARY);
    if (value != NULL) {
        int val_w = ui_text_measure(font_sm, value);
        int x = 224 - val_w;
        if (x < 80) {
            x = 80;
        }
        ui_draw_text(fb, fb_w, fb_h, y_origin, x, y, value,
                     font_sm, val_color);
    }
}

void michi_ui_draw_screen_diagnostics(uint16_t *fb, uint16_t fb_w,
                                      uint16_t fb_h, uint16_t y_origin,
                                      const michi_ui_screen_ctx_t *ctx)
{
    char vol_buf[16];
    char rate_buf[24];
    uint8_t vol = ctx != NULL ? ctx->volume : 50;
    uint32_t rate = (ctx != NULL && ctx->sample_rate > 0) ? ctx->sample_rate : 48000u;
    uint8_t bits = (ctx != NULL && ctx->bit_depth > 0) ? ctx->bit_depth : 16u;

    snprintf(vol_buf, sizeof(vol_buf), "%u", (unsigned)vol);
    snprintf(rate_buf, sizeof(rate_buf), "%" PRIu32 " kHz / %u-bit",
             rate / 1000u, (unsigned)bits);

    /* Header */
    michi_ui_draw_header(fb, fb_w, fb_h, y_origin, MICHI_UI_STR_DIAGNOSTICS_TITLE);

    int y = 48;
    const int pitch = 26;

    draw_diag_row(fb, fb_w, fb_h, y_origin, y, "Board", "Waveshare S3-LCD-2", MICHI_UI_TEXT_PRIMARY);
    y += pitch;
    draw_diag_row(fb, fb_w, fb_h, y_origin, y, "Wi-Fi",
                  (ctx != NULL && ctx->wifi_connected) ? "Conectado" : "Desconectado",
                  (ctx != NULL && ctx->wifi_connected) ? MICHI_UI_SUCCESS : MICHI_UI_MUTED);
    y += pitch;
    draw_diag_row(fb, fb_w, fb_h, y_origin, y, "Servidor",
                  (ctx != NULL && ctx->server_connected) ? "Conectado" : "Desconectado",
                  (ctx != NULL && ctx->server_connected) ? MICHI_UI_SUCCESS : MICHI_UI_MUTED);
    y += pitch;
    draw_diag_row(fb, fb_w, fb_h, y_origin, y, "Audio", rate_buf, MICHI_UI_TEXT_PRIMARY);
    y += pitch;
    draw_diag_row(fb, fb_w, fb_h, y_origin, y, "DAC", "PCM5122", MICHI_UI_SUCCESS);
    y += pitch;
    draw_diag_row(fb, fb_w, fb_h, y_origin, y, "Volumen", vol_buf, MICHI_UI_TEXT_PRIMARY);
    y += pitch;
    draw_diag_row(fb, fb_w, fb_h, y_origin, y, "PSRAM", "8 MB", MICHI_UI_TEXT_PRIMARY);
    y += pitch;
    draw_diag_row(fb, fb_w, fb_h, y_origin, y, "Estado",
                  (ctx != NULL && ctx->state == MICHI_STATE_PLAYING) ? "Reproduciendo" : "Listo",
                  MICHI_UI_TEXT_PRIMARY);
}

void michi_ui_render_screen(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                            uint16_t y_origin, const michi_ui_screen_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->show_diagnostics) {
        michi_ui_draw_screen_diagnostics(fb, fb_w, fb_h, y_origin, ctx);
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

    case MICHI_STATE_PROVISIONING:
        michi_ui_draw_screen_provisioning(fb, fb_w, fb_h, y_origin);
        break;

    case MICHI_STATE_WIFI_CONNECTING:
        michi_ui_draw_screen_connecting(fb, fb_w, fb_h, y_origin, NULL);
        break;

    case MICHI_STATE_IDLE:
        michi_ui_draw_screen_ready(fb, fb_w, fb_h, y_origin, ctx);
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
        michi_ui_draw_screen_ready(fb, fb_w, fb_h, y_origin, ctx);
        break;
    }
}
