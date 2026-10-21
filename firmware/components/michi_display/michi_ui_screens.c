#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "michi_ui.h"
#include "michi_ui_components.h"
#include "michi_ui_screens.h"
#include "michi_ui_strings.h"
#include "michi_ui_text.h"
#include "michi_version.h"

michi_error_class_t michi_ui_classify_error(uint32_t error_code)
{
    if (error_code == 0) {
        return MICHI_ERR_CLASS_NONE;
    }
    if (error_code == 0x101) {
        return MICHI_ERR_CLASS_MEMORY;
    }
    if (error_code == 0x103 || error_code == 0x107 || (error_code >= 0x7000 && error_code <= 0x7FFF)) {
        return MICHI_ERR_CLASS_AUDIO;
    }
    if (error_code >= 0x3000 && error_code <= 0x5FFF) {
        return MICHI_ERR_CLASS_NETWORK;
    }
    if (error_code >= 0x2000 && error_code <= 0x2FFF) {
        return MICHI_ERR_CLASS_STORAGE;
    }
    if (error_code >= 0x6000 && error_code <= 0x6FFF) {
        return MICHI_ERR_CLASS_UPDATE;
    }
    return MICHI_ERR_CLASS_UNKNOWN;
}

const char *michi_ui_error_code_str(uint32_t error_code)
{
    michi_error_class_t cl = michi_ui_classify_error(error_code);
    switch (cl) {
    case MICHI_ERR_CLASS_NETWORK: return "E101";
    case MICHI_ERR_CLASS_AUDIO:   return "E102";
    case MICHI_ERR_CLASS_STORAGE: return "E103";
    case MICHI_ERR_CLASS_MEMORY:  return "E104";
    case MICHI_ERR_CLASS_UPDATE:  return "E105";
    default:                      return "E199";
    }
}


/* Signature Hi-Fi UI-02: accent copper comes from the official SVG logo.
 * Both are semantic tokens; no per-screen color literals. */
#define UI_BRAND_COPPER 0xFC0Cu /* warm copper inspired by official SVG */
#define UI_BRAND_SOFT 0xC2A9u

/* Draw only pixels in the requested DMA band. No alpha buffer or heap. */
static void ui_panel(uint16_t *fb, uint16_t fw, uint16_t fh, uint16_t oy,
                     int x, int y, int w, int h, uint16_t fill, uint16_t outline)
{
    if (!fb || w <= 0 || h <= 0) return;
    int first = y > (int)oy ? y : (int)oy;
    int last = y+h < (int)oy+(int)fh ? y+h : (int)oy+(int)fh;
    for (int yy=first; yy<last; ++yy) {
        const bool edge = yy==y || yy==y+h-1;
        michi_ui_draw_hline(fb,fw,fh,oy,x,yy,w,edge?outline:fill);
        if (!edge) {
            michi_ui_draw_hline(fb,fw,fh,oy,x,yy,1,outline);
            michi_ui_draw_hline(fb,fw,fh,oy,x+w-1,yy,1,outline);
        }
    }
}
static void ui_text_fit(uint16_t *fb,uint16_t fw,uint16_t fh,uint16_t oy,
                        int x,int y,int max_width,const char *src,
                        michi_ui_font_id_t font_id,uint16_t color)
{
    if (!src) return;
    char buff[128];
    const michi_ui_font_t *font=michi_ui_font_get(font_id);
    michi_ui_utf8_safe_copy(buff,sizeof(buff),src);
    (void)ui_ellipsize(font,buff,max_width);
    ui_draw_text(fb,fw,fh,oy,x,y,buff,font,color);
}
static void ui_kicker(uint16_t *fb,uint16_t fw,uint16_t fh,uint16_t oy,
                      int x,int y,const char *text,uint16_t color)
{
    michi_ui_draw_hline(fb,fw,fh,oy,x,y+5,12,color);
    ui_text_fit(fb,fw,fh,oy,x+19,y,220,text,MICHI_FONT_XS,MICHI_UI_TEXT_SECONDARY);
}
static void ui_hero(uint16_t *fb,uint16_t fw,uint16_t fh,uint16_t oy,
                    const char *title,const char *subtitle,uint16_t color)
{
    michi_ui_draw_brand(fb,fw,fh,oy,124,45,72);
    ui_draw_text_centered(fb,fw,fh,oy,132,title,michi_ui_font_get(MICHI_FONT_LG),MICHI_UI_TEXT_PRIMARY);
    ui_draw_text_centered(fb,fw,fh,oy,170,subtitle,michi_ui_font_get(MICHI_FONT_SM),color);
}
static void ui_diag_row(uint16_t *fb,uint16_t fw,uint16_t fh,uint16_t oy,
                        int x,int y,const char *key,const char *value,uint16_t color)
{
    ui_text_fit(fb,fw,fh,oy,x,y,132,key,MICHI_FONT_XS,MICHI_UI_TEXT_SECONDARY);
    ui_text_fit(fb,fw,fh,oy,x,y+10,132,value,MICHI_FONT_SM,color);
}

static void format_audio_spec(char *dst, size_t dst_cap, uint32_t sample_rate, uint8_t bit_depth)
{
    if (sample_rate == 0 || bit_depth == 0) {
        snprintf(dst, dst_cap, "Audio sin confirmar");
    } else {
        if (sample_rate % 1000u == 0u) {
            snprintf(dst, dst_cap, "%" PRIu32 " kHz / %u bit", sample_rate / 1000u, (unsigned)bit_depth);
        } else if (sample_rate % 100u == 0u) {
            snprintf(dst, dst_cap, "%" PRIu32 ".%" PRIu32 " kHz / %u bit", sample_rate / 1000u,
                     (sample_rate % 1000u) / 100u, (unsigned)bit_depth);
        } else {
            snprintf(dst, dst_cap, "%" PRIu32 ".%03" PRIu32 " kHz / %u bit", sample_rate / 1000u,
                     sample_rate % 1000u, (unsigned)bit_depth);
        }
    }
}

/* UI-04: antialiased Woven Wave — optical polish, not PCM metering.
 * All arithmetic uses fixed point. No full framebuffer, floating point,
 * FFT or heap. Critically, skip the five DMA bands outside the wave region. */
static const int8_t s_wave_sine[64] = {
     0, 12, 25, 37, 49, 60, 71, 81, 90, 98,106,112,117,122,125,126,
   127,126,125,122,117,112,106, 98, 90, 81, 71, 60, 49, 37, 25, 12,
     0,-12,-25,-37,-49,-60,-71,-81,-90,-98,-106,-112,-117,-122,-125,-126,
   -127,-126,-125,-122,-117,-112,-106,-98,-90,-81,-71,-60,-49,-37,-25,-12
};

static int wave_sample_q2(int phase_q2)
{
    /* Interpolate table values between samples, avoiding 1px stair-steps. */
    unsigned p = (unsigned)phase_q2 & 255u;
    int a = (int)s_wave_sine[(p >> 2) & 63u];
    int b = (int)s_wave_sine[((p >> 2) + 1u) & 63u];
    return a * (4 - (int)(p & 3u)) / 4 + b * (int)(p & 3u) / 4;
}

static uint16_t blend_wave_pixel(uint16_t base, uint16_t foreground, unsigned alpha)
{
    /* 4-bit coverage. Pixel is already in the active RGB565 DMA band. */
    unsigned inverse = 16u - alpha;
    unsigned r = (((base >> 11) & 31u) * inverse +
                  ((foreground >> 11) & 31u) * alpha + 8u) >> 4;
    unsigned g = (((base >> 5) & 63u) * inverse +
                  ((foreground >> 5) & 63u) * alpha + 8u) >> 4;
    unsigned b = ((base & 31u) * inverse +
                  (foreground & 31u) * alpha + 8u) >> 4;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void wave_plot_aa(uint16_t *fb, uint16_t fw, uint16_t fh, uint16_t oy,
                         int x, int y, unsigned alpha, uint16_t color)
{
    if (alpha == 0u || x < 0 || x >= (int)fw ||
        y < (int)oy || y >= (int)oy + (int)fh) return;
    size_t pixel = (size_t)(y - (int)oy) * fw + (size_t)x;
    fb[pixel] = blend_wave_pixel(fb[pixel], color, alpha);
}

static void draw_waveform(uint16_t *fb, uint16_t fw, uint16_t fh, uint16_t oy,
                          uint8_t phase, bool moving, bool buffering)
{
    /* Wave pixels are restricted to y=164..190. 5 of 6 DMA bands exit fast. */
    if (!fb || (int)oy >= 191 || (int)oy + (int)fh <= 163) return;
    /* Airy, monochrome four-thread interference; phase derives from UI clock,
     * not PCM or a fake progress value. Paused freezes at a neutral frame. */
    static const uint16_t tones[4] = { 0x632Eu, 0x8412u, 0xA576u, 0x7BB1u };
    int shift = moving ? (int)phase * (buffering ? 3 : 5) : 0;
    for (int layer = 0; layer < 4; ++layer) {
        int amplitude = moving ? (buffering ? 4 + layer : 5 + layer) : 1 + (layer / 3);
        unsigned max_alpha = moving ? (buffering ? 8u + (unsigned)layer : 10u + (unsigned)layer)
                                    : 4u;
        for (int x = 20; x <= 299; ++x) {
            int margin = x - 20 < 299 - x ? x - 20 : 299 - x;
            int envelope = margin >= 54 ? 100 : margin * 100 / 54;
            int p1 = x * 3 + shift + layer * 36;
            int p2 = x * 2 - shift / 2 + layer * 72;
            int oscillation = (wave_sample_q2(p1) * 2 + wave_sample_q2(p2)) / 3;
            /* q4 subpixel Y, centred inside a reserved wave lane. */
            int y_q4 = 177 * 16 + envelope * amplitude * oscillation * 16 / (100 * 127);
            int y = y_q4 / 16;
            unsigned fraction = (unsigned)(y_q4 - y * 16);
            unsigned upper = (max_alpha * (16u - fraction) + 8u) >> 4;
            unsigned lower = (max_alpha * fraction + 8u) >> 4;
            wave_plot_aa(fb, fw, fh, oy, x, y, upper, tones[layer]);
            wave_plot_aa(fb, fw, fh, oy, x, y + 1, lower, tones[layer]);
        }
    }
}

static void draw_source_label(uint16_t *fb, uint16_t fw, uint16_t fh,
                              uint16_t oy, const char *source)
{
    const michi_ui_font_t *font = michi_ui_font_get(MICHI_FONT_SM);
    char safe[64];
    if (source == NULL || source[0] == '\0') return;
    michi_ui_utf8_safe_copy(safe, sizeof(safe), source);
    (void)ui_ellipsize(font, safe, 118);
    const int width = ui_text_measure(font, safe);
    const int text_x = (int)fw - 16 - width;
    const int icon_x = text_x - 15;
    /* Opposite side of playback status; leaves the footer to volume/format. */
    michi_ui_draw_icon(fb, fw, fh, oy, icon_x, 57, MICHI_ICON_ROOM, 12,
                       MICHI_UI_TEXT_TERTIARY);
    ui_draw_text(fb, fw, fh, oy, text_x, 56, safe, font, MICHI_UI_TEXT_SECONDARY);
}

static void draw_playback_chrome(uint16_t *fb, uint16_t fw, uint16_t fh,
                                 uint16_t oy, const michi_ui_screen_ctx_t *ctx,
                                 const char *label, uint16_t status_color)
{
    const michi_ui_font_t *font = michi_ui_font_get(MICHI_FONT_SM);
    const bool moving = ctx && (ctx->state == MICHI_STATE_PLAYING ||
                                ctx->state == MICHI_STATE_BUFFERING);
    const bool buffering = ctx && ctx->state == MICHI_STATE_BUFFERING;
    michi_ui_draw_circle_filled(fb, fw, fh, oy, 22, 63, 3, status_color);
    ui_draw_text(fb, fw, fh, oy, 34, 56, label, font, MICHI_UI_TEXT_SECONDARY);
    draw_waveform(fb, fw, fh, oy, ctx ? ctx->waveform_phase : 0, moving, buffering);
    draw_source_label(fb, fw, fh, oy, ctx ? ctx->source : NULL);
    char fmt[40];
    format_audio_spec(fmt, sizeof(fmt), ctx ? ctx->sample_rate : 0u,
                      ctx ? ctx->bit_depth : 0u);
    michi_ui_draw_playback_footer_landscape(fb, fw, fh, oy,
                                            ctx ? ctx->volume : 0u, fmt);
}

static int draw_track_info(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                            uint16_t y_origin, const michi_ui_screen_ctx_t *ctx,
                            uint16_t title_color, uint16_t artist_color)
{
    if (ctx == NULL || ctx->title == NULL || ctx->title[0] == '\0') return 0;
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);
    const michi_ui_font_t *font_md = michi_ui_font_get(MICHI_FONT_MD);
    char title_buf[128];
    const char *lines[2];
    bool truncated = false;
    michi_ui_utf8_safe_copy(title_buf, sizeof(title_buf), ctx->title);
    int n = ui_wrap_text_ex(font_lg, title_buf, 272, lines, 2, &truncated);
    /* Optical baseline grid. Two-line titles gain 7px above the wave, so
     * the artist never touches decorative motion even at maximum amplitude. */
    int title_y = (n > 1) ? 77 : 85;
    for (int i = 0; i < n; ++i) {
        ui_draw_text(fb, fb_w, fb_h, y_origin, 28,
                     title_y + i * michi_ui_font_line_height(font_lg),
                     lines[i], font_lg, title_color);
    }
    if (ctx->artist != NULL && ctx->artist[0] != '\0') {
        char artist_buf[128];
        michi_ui_utf8_safe_copy(artist_buf, sizeof(artist_buf), ctx->artist);
        (void)ui_ellipsize(font_md, artist_buf, 272);
        int artist_y = (n > 1) ? 141 : 123;
        ui_draw_text(fb, fb_w, fb_h, y_origin, 28, artist_y,
                     artist_buf, font_md, artist_color);
    }
    return n;
}

void michi_ui_draw_screen_boot(uint16_t *fb, uint16_t fw, uint16_t fh, uint16_t oy)
{
    ui_hero(fb,fw,fh,oy,"michi","Iniciando receptor",MICHI_UI_TEXT_SECONDARY);
    michi_ui_draw_activity_dots(fb,fw,fh,oy,160,206,3,13,UI_BRAND_COPPER);
}


void michi_ui_draw_screen_unprovisioned(uint16_t *fb,uint16_t fw,uint16_t fh,uint16_t oy)
{
    /* Composition: the chrome (brand + status + divider) already owns the top
     * band, so this screen starts below it. The old kicker at y=16 drew over
     * the header, and the second instruction used the 8 px XS face, which is
     * unreadable at arm's length on a 2" panel. Everything essential is SM
     * (13 px) now, in short lines instead of one long truncated sentence. */
    michi_ui_draw_brand(fb,fw,fh,oy,144,54,32);
    ui_draw_text_centered(fb,fw,fh,oy,94,"Prepara tu Michi",michi_ui_font_get(MICHI_FONT_LG),MICHI_UI_TEXT_PRIMARY);

    ui_panel(fb,fw,fh,oy,16,136,288,92,MICHI_UI_SURFACE,MICHI_UI_SURFACE_ELEVATED);
    /* BODY (15 px): SM measured too small at arm's length on the real panel. */
    ui_text_fit(fb,fw,fh,oy,28,145,264,"Conecta el dispositivo por USB",MICHI_FONT_BODY,MICHI_UI_TEXT_PRIMARY);
    ui_text_fit(fb,fw,fh,oy,28,164,264,"y configúralo en Michi Ecosystem.",MICHI_FONT_BODY,MICHI_UI_TEXT_PRIMARY);
    ui_text_fit(fb,fw,fh,oy,28,183,264,"Wi-Fi e identidad se configuran",MICHI_FONT_BODY,MICHI_UI_TEXT_SECONDARY);
    ui_text_fit(fb,fw,fh,oy,28,202,264,"desde tu computador.",MICHI_FONT_BODY,MICHI_UI_TEXT_SECONDARY);
}


void michi_ui_draw_screen_ready(uint16_t *fb,uint16_t fw,uint16_t fh,uint16_t oy,
                                const michi_ui_screen_ctx_t *ctx)
{
    bool wifi=ctx && ctx->wifi_connected;
    bool ip=ctx && ctx->network_ready;
    const char *message=!wifi?"Sin conexión Wi-Fi":(!ip?"Esperando dirección IP":"Listo para reproducir");
    uint16_t state_color=!wifi?MICHI_UI_WARNING:(!ip?MICHI_UI_INFO:MICHI_UI_SUCCESS);
    ui_kicker(fb,fw,fh,oy,18,55,"RECEPTOR DE AUDIO",UI_BRAND_COPPER);
    michi_ui_draw_brand(fb,fw,fh,oy,140,81,40);
    ui_draw_text_centered(fb,fw,fh,oy,126,"Michi Stream",michi_ui_font_get(MICHI_FONT_LG),MICHI_UI_TEXT_PRIMARY);
    ui_panel(fb,fw,fh,oy,16,170,288,36,MICHI_UI_SURFACE,MICHI_UI_SURFACE_ELEVATED);
    michi_ui_draw_circle_filled(fb,fw,fh,oy,33,187,3,state_color);
    ui_text_fit(fb,fw,fh,oy,45,179,240,message,MICHI_FONT_SM,MICHI_UI_TEXT_SECONDARY);
    /* Always-visible field service ribbon: truthful hardware stages, even if
     * the diagnostics mode is not externally exposed on this board. */
    const bool clock=ctx && ctx->clock_synced;
    const bool mdns=ctx && ctx->mdns_running;
    const bool announced=ctx && ctx->announces_sent_session>0 && clock;
    michi_ui_draw_circle_filled(fb,fw,fh,oy,21,221,2,ip?MICHI_UI_SUCCESS:MICHI_UI_WARNING);
    ui_text_fit(fb,fw,fh,oy,28,214,46,"IP",MICHI_FONT_SM,MICHI_UI_TEXT_TERTIARY);
    michi_ui_draw_circle_filled(fb,fw,fh,oy,79,221,2,clock?MICHI_UI_SUCCESS:MICHI_UI_WARNING);
    ui_text_fit(fb,fw,fh,oy,86,214,48,"SNTP",MICHI_FONT_SM,MICHI_UI_TEXT_TERTIARY);
    michi_ui_draw_circle_filled(fb,fw,fh,oy,156,221,2,mdns?MICHI_UI_SUCCESS:MICHI_UI_WARNING);
    ui_text_fit(fb,fw,fh,oy,163,214,49,"mDNS",MICHI_FONT_SM,MICHI_UI_TEXT_TERTIARY);
    michi_ui_draw_circle_filled(fb,fw,fh,oy,240,221,2,announced?MICHI_UI_SUCCESS:MICHI_UI_WARNING);
    ui_text_fit(fb,fw,fh,oy,247,214,56,"Whisker",MICHI_FONT_SM,MICHI_UI_TEXT_TERTIARY);
}


void michi_ui_draw_screen_provisioning(uint16_t *fb,uint16_t fw,uint16_t fh,uint16_t oy)
{
    michi_ui_draw_brand(fb,fw,fh,oy,140,65,40);
    ui_draw_text_centered(fb,fw,fh,oy,122,"Configurando",michi_ui_font_get(MICHI_FONT_LG),MICHI_UI_TEXT_PRIMARY);
    ui_panel(fb,fw,fh,oy,16,165,288,44,MICHI_UI_SURFACE,MICHI_UI_SURFACE_ELEVATED);
    ui_text_fit(fb,fw,fh,oy,28,174,260,"Importando red e identidad Michi Home",MICHI_FONT_SM,MICHI_UI_TEXT_SECONDARY);
    ui_text_fit(fb,fw,fh,oy,28,192,260,"No desconectes la alimentación",MICHI_FONT_SM,MICHI_UI_TEXT_TERTIARY);
}


void michi_ui_draw_screen_connecting(uint16_t *fb,uint16_t fw,uint16_t fh,uint16_t oy,
                                     const char *ssid,const michi_ui_screen_ctx_t *ctx)
{
    (void)ctx;
    ui_kicker(fb,fw,fh,oy,18,55,"CONECTIVIDAD",MICHI_UI_INFO);
    michi_ui_draw_icon(fb,fw,fh,oy,144,84,MICHI_ICON_WIFI,32,UI_BRAND_COPPER);
    ui_draw_text_centered(fb,fw,fh,oy,129,"Conectando",michi_ui_font_get(MICHI_FONT_LG),MICHI_UI_TEXT_PRIMARY);
    ui_panel(fb,fw,fh,oy,16,170,288,37,MICHI_UI_SURFACE,MICHI_UI_SURFACE_ELEVATED);
    ui_text_fit(fb,fw,fh,oy,28,182,258,(ssid&&ssid[0])?ssid:"Buscando red Wi-Fi",MICHI_FONT_SM,MICHI_UI_TEXT_SECONDARY);
}


void michi_ui_draw_screen_pairing(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin,
                                  const char *pin, const michi_ui_screen_ctx_t *ctx)
{
    (void)ctx;
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);
    (void)font_sm;

    if (pin != NULL && pin[0] != '\0') {
        ui_draw_text(fb, fb_w, fb_h, y_origin, 18, 20, MICHI_UI_STR_PAIRING_TITLE, font_lg, MICHI_UI_TEXT_PRIMARY);
        michi_ui_draw_pin_landscape(fb, fb_w, fb_h, y_origin, 230, 115, pin, MICHI_UI_ACCENT);
        ui_draw_text(fb, fb_w, fb_h, y_origin, 18, 110, "Introduce el código", font_sm, MICHI_UI_TEXT_SECONDARY);
        ui_draw_text(fb, fb_w, fb_h, y_origin, 18, 128, "en Michi", font_sm, MICHI_UI_TEXT_SECONDARY);
        michi_ui_draw_activity_dots(fb, fb_w, fb_h, y_origin, 230, 175, 5, 12, MICHI_UI_ACCENT);
    } else {
        if (ctx == NULL || !ctx->wifi_connected || !ctx->network_ready) {
            ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 90, MICHI_UI_STR_PAIRING_TITLE, font_lg, MICHI_UI_TEXT_PRIMARY);
            ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 122, MICHI_UI_STR_NO_CONNECTION, font_sm, MICHI_UI_TEXT_SECONDARY);
        } else {
            ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 90, MICHI_UI_STR_PAIRING_WAITING_TITLE, font_lg, MICHI_UI_TEXT_PRIMARY);
            ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 122, MICHI_UI_STR_PAIRING_WAITING_HINT, font_sm, MICHI_UI_TEXT_SECONDARY);
            michi_ui_draw_activity_dots(fb, fb_w, fb_h, y_origin, 160, 160, 3, 12, MICHI_UI_ACCENT);
        }
    }
}

void michi_ui_draw_screen_session_pending(uint16_t *fb,uint16_t fw,uint16_t fh,uint16_t oy,
                                         const michi_ui_screen_ctx_t *ctx)
{
    ui_kicker(fb,fw,fh,oy,18,55,"SESION / CONTROL",MICHI_UI_INFO);
    michi_ui_draw_icon(fb,fw,fh,oy,144,83,MICHI_ICON_WAVE,32,UI_BRAND_COPPER);
    ui_draw_text_centered(fb,fw,fh,oy,126,"Preparando audio",michi_ui_font_get(MICHI_FONT_LG),MICHI_UI_TEXT_PRIMARY);
    ui_panel(fb,fw,fh,oy,16,167,288,38,MICHI_UI_SURFACE,MICHI_UI_SURFACE_ELEVATED);
    ui_text_fit(fb,fw,fh,oy,28,179,256,ctx&&ctx->server_connected?"Sesión establecida":"Esperando sesión autorizada",MICHI_FONT_SM,MICHI_UI_TEXT_SECONDARY);
}


void michi_ui_draw_screen_buffering(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin,
                                    const michi_ui_screen_ctx_t *ctx)
{
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);
    draw_playback_chrome(fb, fb_w, fb_h, y_origin, ctx,
                         MICHI_UI_STR_AUDIO_PREP, MICHI_UI_INFO);
    int n = draw_track_info(fb, fb_w, fb_h, y_origin, ctx,
                            MICHI_UI_TEXT_PRIMARY, MICHI_UI_TEXT_SECONDARY);
    if (n == 0) {
        michi_ui_draw_icon(fb, fb_w, fb_h, y_origin, 144, 85, MICHI_ICON_WAVE, 32,
                           MICHI_UI_INFO);
        ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 130,
                              MICHI_UI_STR_AUDIO_PREP, font_lg, MICHI_UI_TEXT_PRIMARY);
    } else {
        /* The woven wave communicates buffering activity in this state. */
    }
    (void)font_sm;
}

void michi_ui_draw_screen_playing(uint16_t *fb,uint16_t fw,uint16_t fh,uint16_t oy,
                                 const michi_ui_screen_ctx_t *ctx)
{
    draw_playback_chrome(fb,fw,fh,oy,ctx,MICHI_UI_STR_PLAYING,MICHI_UI_SUCCESS);
    if (draw_track_info(fb,fw,fh,oy,ctx,MICHI_UI_TEXT_PRIMARY,MICHI_UI_TEXT_SECONDARY)==0) {
        michi_ui_draw_icon(fb,fw,fh,oy,141,95,MICHI_ICON_PLAY,32,UI_BRAND_COPPER);
        ui_draw_text_centered(fb,fw,fh,oy,133,"Audio en reproducción",michi_ui_font_get(MICHI_FONT_MD),MICHI_UI_TEXT_PRIMARY);
    }
}


void michi_ui_draw_screen_paused(uint16_t *fb,uint16_t fw,uint16_t fh,uint16_t oy,
                                const michi_ui_screen_ctx_t *ctx)
{
    draw_playback_chrome(fb,fw,fh,oy,ctx,MICHI_UI_STR_PAUSED,MICHI_UI_WARNING);
    if (draw_track_info(fb,fw,fh,oy,ctx,MICHI_UI_TEXT_SECONDARY,MICHI_UI_TEXT_TERTIARY)==0) {
        michi_ui_draw_icon(fb,fw,fh,oy,144,94,MICHI_ICON_PAUSE,32,MICHI_UI_WARNING);
        ui_draw_text_centered(fb,fw,fh,oy,133,"Reproducción en pausa",michi_ui_font_get(MICHI_FONT_MD),MICHI_UI_TEXT_PRIMARY);
    }
}


void michi_ui_draw_screen_updating(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, uint16_t y_origin,
                                   uint8_t pct, bool has_pct, const michi_ui_screen_ctx_t *ctx)
{
    (void)ctx;
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);
    (void)font_sm;

    michi_ui_draw_icon(fb, fb_w, fb_h, y_origin, 18, 85, MICHI_ICON_UPDATE, 32, UI_BRAND_COPPER);
    ui_draw_text(fb, fb_w, fb_h, y_origin, 64, 85, MICHI_UI_STR_UPDATING_TITLE, font_lg, MICHI_UI_TEXT_PRIMARY);
    ui_draw_text(fb, fb_w, fb_h, y_origin, 64, 117, MICHI_UI_STR_UPDATING_HINT, font_sm, MICHI_UI_TEXT_SECONDARY);

    if (has_pct) {
        michi_ui_draw_progress_landscape(fb, fb_w, fb_h, y_origin, 18, 160, 284, 3, pct);
        char pct_str[16];
        snprintf(pct_str, sizeof(pct_str), "%u%%", (unsigned)pct);
        ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 175, pct_str, font_sm, MICHI_UI_TEXT_TERTIARY);
    } else {
        michi_ui_draw_hline(fb, fb_w, fb_h, y_origin, 18, 160, 284, MICHI_UI_SURFACE_ELEVATED);
        michi_ui_draw_hline(fb, fb_w, fb_h, y_origin, 100, 160, 60, MICHI_UI_ACCENT);
    }
}

void michi_ui_draw_screen_recoverable_error(uint16_t *fb,uint16_t fw,uint16_t fh,uint16_t oy,
                                          uint32_t error_code,const michi_ui_screen_ctx_t *ctx)
{
    (void)ctx;
    ui_kicker(fb,fw,fh,oy,18,58,"RECUPERACION AUTOMATICA",MICHI_UI_WARNING);
    michi_ui_draw_icon(fb,fw,fh,oy,20,101,MICHI_ICON_WARNING,32,MICHI_UI_WARNING);
    const bool audio=michi_ui_classify_error(error_code)==MICHI_ERR_CLASS_AUDIO;
    ui_text_fit(fb,fw,fh,oy,66,98,233,audio?"Recuperando audio":"Reconectando",MICHI_FONT_LG,MICHI_UI_TEXT_PRIMARY);
    ui_panel(fb,fw,fh,oy,16,155,288,48,MICHI_UI_SURFACE,MICHI_UI_SURFACE_ELEVATED);
    ui_text_fit(fb,fw,fh,oy,28,166,260,audio?"Restableciendo el flujo RTP":"Reconectando a la red",MICHI_FONT_SM,MICHI_UI_TEXT_SECONDARY);
    if(error_code) {
        char code[24];snprintf(code,sizeof(code),"%s / 0x%04" PRIX32,michi_ui_error_code_str(error_code),error_code);
        ui_text_fit(fb,fw,fh,oy,28,184,260,code,MICHI_FONT_XS,MICHI_UI_WARNING);
    }
}


void michi_ui_draw_screen_fatal_error(uint16_t *fb,uint16_t fw,uint16_t fh,uint16_t oy,
                                    uint32_t error_code,const michi_ui_screen_ctx_t *ctx)
{
    (void)ctx;
    ui_kicker(fb,fw,fh,oy,18,56,"ERROR CRITICO",MICHI_UI_ERROR);
    michi_ui_draw_icon(fb,fw,fh,oy,18,100,MICHI_ICON_ERROR,32,MICHI_UI_ERROR);
    ui_text_fit(fb,fw,fh,oy,64,97,235,"Error del sistema",MICHI_FONT_LG,MICHI_UI_TEXT_PRIMARY);
    ui_panel(fb,fw,fh,oy,16,156,288,52,MICHI_UI_SURFACE,MICHI_UI_SURFACE_ELEVATED);
    ui_text_fit(fb,fw,fh,oy,27,165,260,"Reinicia el receptor; revisa diagnóstico",MICHI_FONT_SM,MICHI_UI_TEXT_SECONDARY);
    if(error_code) {
        char code[28];snprintf(code,sizeof(code),"%s / 0x%04" PRIX32,michi_ui_error_code_str(error_code),error_code);
        ui_text_fit(fb,fw,fh,oy,27,186,260,code,MICHI_FONT_SM,MICHI_UI_ERROR);
    }
}


void michi_ui_draw_screen_diagnostics(uint16_t *fb,uint16_t fw,uint16_t fh,uint16_t oy,
                                      const michi_ui_screen_ctx_t *ctx)
{
    ui_text_fit(fb,fw,fh,oy,16,10,250,"DIAGNOSTICO / STREAM",MICHI_FONT_SM,MICHI_UI_TEXT_PRIMARY);
    michi_ui_draw_hline(fb,fw,fh,oy,16,39,288,MICHI_UI_SURFACE_ELEVATED);
    michi_ui_draw_hline(fb,fw,fh,oy,16,39,38,UI_BRAND_COPPER);
    ui_panel(fb,fw,fh,oy,11,47,145,166,MICHI_UI_SURFACE,MICHI_UI_SURFACE_ELEVATED);
    ui_panel(fb,fw,fh,oy,163,47,146,166,MICHI_UI_SURFACE,MICHI_UI_SURFACE_ELEVATED);
    ui_text_fit(fb,fw,fh,oy,21,53,125,"RED Y DISCOVERY",MICHI_FONT_SM,UI_BRAND_COPPER);
    ui_text_fit(fb,fw,fh,oy,173,53,125,"AUDIO Y HARDWARE",MICHI_FONT_SM,UI_BRAND_COPPER);
    michi_ui_draw_hline(fb,fw,fh,oy,21,70,125,MICHI_UI_SURFACE_ELEVATED);
    michi_ui_draw_hline(fb,fw,fh,oy,173,70,125,MICHI_UI_SURFACE_ELEVATED);

    char wifi[40];
    if(ctx&&ctx->wifi_connected) {
        if(ctx->wifi_rssi!=0) snprintf(wifi,sizeof(wifi),"Activo / %d dBm",(int)ctx->wifi_rssi);
        else snprintf(wifi,sizeof(wifi),"Conectado");
    } else snprintf(wifi,sizeof(wifi),"Desconectado");
    const char *ip = (ctx&&ctx->network_ready&&ctx->ipv4_str[0]&&strcmp(ctx->ipv4_str,"0.0.0.0")!=0)
                     ?ctx->ipv4_str:"Sin dirección IP";
    const char *clock = (ctx&&ctx->clock_synced)?"Sincronizado":"Sin sincronizar";
    const char *mdns = (ctx&&ctx->mdns_running)?"Servicio activo":"No anunciado";
    char signed_tx[36];
    if(!ctx||!ctx->network_ready) snprintf(signed_tx,sizeof(signed_tx),"Sin red");
    else if(!ctx->clock_synced) snprintf(signed_tx,sizeof(signed_tx),"Esperando reloj");
    else snprintf(signed_tx,sizeof(signed_tx),"TX firmados: %" PRIu32,ctx->announces_sent_session);

    ui_diag_row(fb,fw,fh,oy,21,74,"Wi-Fi",wifi,ctx&&ctx->wifi_connected?MICHI_UI_SUCCESS:MICHI_UI_WARNING);
    ui_diag_row(fb,fw,fh,oy,21,101,"IPv4",ip,ctx&&ctx->network_ready?MICHI_UI_TEXT_PRIMARY:MICHI_UI_WARNING);
    ui_diag_row(fb,fw,fh,oy,21,128,"Reloj / SNTP",clock,ctx&&ctx->clock_synced?MICHI_UI_SUCCESS:MICHI_UI_WARNING);
    ui_diag_row(fb,fw,fh,oy,21,155,"mDNS",mdns,ctx&&ctx->mdns_running?MICHI_UI_SUCCESS:MICHI_UI_WARNING);
    ui_diag_row(fb,fw,fh,oy,21,182,"Whisker multicast",signed_tx,(ctx&&ctx->announces_sent_session>0&&ctx->clock_synced)?MICHI_UI_SUCCESS:MICHI_UI_WARNING);

    char dac[45], format[35], volume[12], psram[35];
    michi_ui_dac_state_t d=(ctx)?ctx->dac_state:MICHI_UI_DAC_UNKNOWN;
    if(ctx&&d==MICHI_UI_DAC_PRESENT&&ctx->dac_model&&ctx->dac_model[0])
        snprintf(dac,sizeof(dac),"%s",ctx->dac_model);
    else if(d==MICHI_UI_DAC_ABSENT) snprintf(dac,sizeof(dac),"No detectado");
    else if(d==MICHI_UI_DAC_ERROR) snprintf(dac,sizeof(dac),"Error de DAC");
    else snprintf(dac,sizeof(dac),"Desconocido");
    if(ctx&&ctx->server_connected&&ctx->sample_rate&&ctx->bit_depth)
        format_audio_spec(format,sizeof(format),ctx->sample_rate,ctx->bit_depth);
    else snprintf(format,sizeof(format),"Sin negociar");
    if(ctx) snprintf(volume,sizeof(volume),"%u / 100",(unsigned)ctx->volume);
    else snprintf(volume,sizeof(volume),"Desconocido");
    if(ctx&&ctx->psram_bytes) snprintf(psram,sizeof(psram),"%" PRIu32 " MB",ctx->psram_bytes/(1024u*1024u));
    else snprintf(psram,sizeof(psram),"Sin medición");

    ui_diag_row(fb,fw,fh,oy,173,74,"Sesión RTP",ctx&&ctx->server_connected?"Activa":"Sin sesión",
                ctx&&ctx->server_connected?MICHI_UI_SUCCESS:MICHI_UI_TEXT_SECONDARY);
    ui_diag_row(fb,fw,fh,oy,173,101,"DAC detectado",dac,d==MICHI_UI_DAC_PRESENT?MICHI_UI_SUCCESS:(d==MICHI_UI_DAC_ERROR?MICHI_UI_ERROR:MICHI_UI_WARNING));
    ui_diag_row(fb,fw,fh,oy,173,128,"Formato sesión",format,ctx&&ctx->server_connected?MICHI_UI_TEXT_PRIMARY:MICHI_UI_TEXT_TERTIARY);
    ui_diag_row(fb,fw,fh,oy,173,155,"Volumen",volume,MICHI_UI_TEXT_PRIMARY);
    ui_diag_row(fb,fw,fh,oy,173,182,"PSRAM medida",psram,MICHI_UI_TEXT_SECONDARY);

    const char *fwver=(ctx&&ctx->fw_version)?ctx->fw_version:MICHI_FW_PROVENANCE_STR;
    ui_text_fit(fb,fw,fh,oy,16,222,173,fwver,MICHI_FONT_XS,MICHI_UI_TEXT_TERTIARY);
    if(ctx && ctx->last_error) {
        char err[25];
        snprintf(err,sizeof(err),"%s 0x%04" PRIX32,michi_ui_error_code_str(ctx->last_error),ctx->last_error);
        ui_text_fit(fb,fw,fh,oy,193,222,109,err,MICHI_FONT_XS,MICHI_UI_WARNING);
    }
}


void michi_ui_draw_screen_button_press_feedback(uint16_t *fb, uint16_t fb_w,
                                                uint16_t fb_h, uint16_t y_origin)
{
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);

    /* Physical button icon 48x48 centered at cx=160, y=65 */
    michi_ui_draw_icon(fb, fb_w, fb_h, y_origin, (320 - 48) / 2, 65, MICHI_ICON_BUTTON, 48,
                       MICHI_UI_ACCENT);

    /* Primary instruction centered at y=130 */
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 130,
                          MICHI_UI_STR_BTN_RELEASE_HINT, font_lg, MICHI_UI_TEXT_PRIMARY);

    /* Subtext centered at y=158 in SM */
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 158,
                          MICHI_UI_STR_BTN_RELEASE_SUB, font_sm, MICHI_UI_TEXT_SECONDARY);
}

void michi_ui_draw_screen_pairing_no_network(uint16_t *fb, uint16_t fb_w,
                                             uint16_t fb_h, uint16_t y_origin)
{
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);
    const michi_ui_font_t *font_sm = michi_ui_font_get(MICHI_FONT_SM);

    /* Warning icon 32x32 centered at cx=160, y=65 */
    michi_ui_draw_icon(fb, fb_w, fb_h, y_origin, (320 - 32) / 2, 65, MICHI_ICON_WARNING, 32,
                       MICHI_UI_WARNING);

    /* Title "Sin conexión" centered at y=120 */
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 120,
                          MICHI_UI_STR_NO_CONNECTION, font_lg, MICHI_UI_TEXT_PRIMARY);

    /* Subtitle "Esperando dirección IP" centered at y=152 */
    ui_draw_text_centered(fb, fb_w, fb_h, y_origin, 152,
                          "Esperando direcci\xc3\xb3n IP", font_sm, MICHI_UI_TEXT_SECONDARY);
}

/* Header + divider chrome shared by the state screens and by the pairing
 * overlays that stand in for a state screen.
 *
 * The Wi-Fi indicator reports usable network readiness (link AND address),
 * not bare association: a link that never got an IP cannot carry a session,
 * so advertising it as connected would contradict the "Sin conexión" status
 * these screens show. Passing the raw wifi_connected made the PAIRING screen
 * differ between "no Wi-Fi" and "Wi-Fi without IP", which the network-truth
 * contract forbids. */
static void draw_screen_chrome(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                               uint16_t y_origin, const michi_ui_screen_ctx_t *ctx,
                               bool is_playing)
{
    michi_ui_draw_header_landscape(fb, fb_w, fb_h, y_origin, NULL,
                                   ctx->wifi_connected && ctx->network_ready,
                                   ctx->wifi_rssi, ctx->server_connected);
    michi_ui_draw_divider(fb, fb_w, fb_h, y_origin, is_playing);
}

void michi_ui_render_screen(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                            uint16_t y_origin, const michi_ui_screen_ctx_t *ctx)
{
    if (fb == NULL || fb_w == 0 || fb_h == 0) {
        return;
    }

#if CONFIG_MICHI_UI_CALIBRATION
    /* Lab build: every band renders the calibration card instead of the UI. */
    michi_ui_draw_calibration(fb, fb_w, fb_h, y_origin);
    return;
#endif

    /* Every band is cleared to the canonical RGB565 background. */
    ui_clear_band(fb, fb_w, fb_h, MICHI_UI_BG);

    if (ctx == NULL) {
        return;
    }

    /* FATAL error: critical protected screen, always beats all overlays and diagnostics */
    if (ctx->state == MICHI_STATE_FATAL_ERROR) {
        michi_ui_draw_screen_fatal_error(fb, fb_w, fb_h, y_origin, ctx->last_error, ctx);
        return;
    }

    /* UPDATING: critical OTA progress, cannot be obscured by temporary overlays */
    if (ctx->state == MICHI_STATE_UPDATING) {
        michi_ui_draw_header_landscape(fb, fb_w, fb_h, y_origin, NULL,
                                        ctx->wifi_connected, ctx->wifi_rssi, ctx->server_connected);
        michi_ui_draw_divider(fb, fb_w, fb_h, y_origin, false);
        michi_ui_draw_screen_updating(fb, fb_w, fb_h, y_origin, ctx->update_pct, ctx->has_update_pct, ctx);
        return;
    }

    /* Diagnostics is subordinate to OTA and fatal safety screens. */
    /* Diagnostics: beats non-fatal overlays */
    if (ctx->show_diagnostics) {
        michi_ui_draw_screen_diagnostics(fb, fb_w, fb_h, y_origin, ctx);
        return;
    }

    /* Pairing overlays: beat volume; primary FSM state is preserved.
     * PIN and WAITING both render the PAIRING screen, so they carry the same
     * chrome that screen gets on its own; otherwise the overlay would differ
     * from the state screen purely because the dispatcher returns early. */
    if (ctx->pairing_overlay == MICHI_UI_PAIRING_OVERLAY_PIN ||
        ctx->pairing_overlay == MICHI_UI_PAIRING_OVERLAY_WAITING) {
        draw_screen_chrome(fb, fb_w, fb_h, y_origin, ctx, false);
        michi_ui_draw_screen_pairing(
            fb, fb_w, fb_h, y_origin,
            ctx->pairing_overlay == MICHI_UI_PAIRING_OVERLAY_PIN ? ctx->pairing_pin : NULL,
            ctx);
        return;
    }
    if (ctx->pairing_overlay == MICHI_UI_PAIRING_OVERLAY_BUTTON_PRESS) {
        michi_ui_draw_screen_button_press_feedback(fb, fb_w, fb_h, y_origin);
        return;
    }
    if (ctx->pairing_overlay == MICHI_UI_PAIRING_OVERLAY_NO_NETWORK) {
        michi_ui_draw_screen_pairing_no_network(fb, fb_w, fb_h, y_origin);
        return;
    }

    /* Temporary volume overlay */
    if (ctx->show_volume_overlay) {
        michi_ui_draw_volume_overlay(fb, fb_w, fb_h, y_origin, ctx->volume);
        return;
    }

    if (ctx->state != MICHI_STATE_BOOTING && ctx->state != MICHI_STATE_SELF_TEST) {
        draw_screen_chrome(fb, fb_w, fb_h, y_origin, ctx, false);
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
        michi_ui_draw_screen_connecting(fb, fb_w, fb_h, y_origin, ctx->wifi_ssid, ctx);
        break;
    case MICHI_STATE_PAIRING:
        michi_ui_draw_screen_pairing(fb, fb_w, fb_h, y_origin, ctx->pairing_pin, ctx);
        break;
    case MICHI_STATE_SESSION_PENDING:
        michi_ui_draw_screen_session_pending(fb, fb_w, fb_h, y_origin, ctx);
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
        michi_ui_draw_screen_updating(fb, fb_w, fb_h, y_origin, ctx->update_pct, ctx->has_update_pct, ctx);
        break;
    case MICHI_STATE_RECOVERABLE_ERROR:
        michi_ui_draw_screen_recoverable_error(fb, fb_w, fb_h, y_origin, ctx->last_error, ctx);
        break;
    case MICHI_STATE_FATAL_ERROR:
        michi_ui_draw_screen_fatal_error(fb, fb_w, fb_h, y_origin, ctx->last_error, ctx);
        break;
    default:
        michi_ui_draw_screen_boot(fb, fb_w, fb_h, y_origin);
        break;
    }
}

#if CONFIG_MICHI_UI_CALIBRATION
/* ---------------------------------------------------------------------------
 * Colour calibration card (lab builds only).
 *
 * Deliberately independent of the Michi palette: the values here are the
 * RGB565 anchors the panel must reproduce, so a wrong byte order or a wrong
 * colour mode is visible at a glance instead of being masked by theme colours.
 * ------------------------------------------------------------------------- */

static void cal_rect(uint16_t *fb, uint16_t fw, uint16_t fh, uint16_t oy,
                     int x, int y, int w, int h, uint16_t color)
{
    if (fb == NULL) {
        return;
    }
    int y0 = y < (int)oy ? (int)oy : y;
    int y1 = y + h;
    if (y1 > (int)oy + (int)fh) {
        y1 = (int)oy + (int)fh;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (x + w > (int)fw) {
        w = (int)fw - x;
    }
    if (w <= 0 || y1 <= y0) {
        return;
    }
    for (int row = y0; row < y1; row++) {
        uint16_t *dst = fb + (size_t)(row - (int)oy) * fw + x;
        for (int i = 0; i < w; i++) {
            dst[i] = color;
        }
    }
}

void michi_ui_draw_calibration(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                               uint16_t y_origin)
{
    ui_clear_band(fb, fb_w, fb_h, 0x0000); /* true black canvas */

    /* Anchors: #FF0000 #00FF00 #0000FF #FFFFFF #808080 */
    cal_rect(fb, fb_w, fb_h, y_origin, 8, 30, 58, 34, 0xF800);
    cal_rect(fb, fb_w, fb_h, y_origin, 70, 30, 58, 34, 0x07E0);
    cal_rect(fb, fb_w, fb_h, y_origin, 132, 30, 58, 34, 0x001F);
    cal_rect(fb, fb_w, fb_h, y_origin, 194, 30, 58, 34, 0xFFFF);
    cal_rect(fb, fb_w, fb_h, y_origin, 256, 30, 56, 34, 0x8410);

    /* #FFFF00 #00FFFF #FF00FF + near-black #080A0F framed on grey */
    cal_rect(fb, fb_w, fb_h, y_origin, 8, 68, 58, 30, 0xFFE0);
    cal_rect(fb, fb_w, fb_h, y_origin, 70, 68, 58, 30, 0x07FF);
    cal_rect(fb, fb_w, fb_h, y_origin, 132, 68, 58, 30, 0xF81F);
    cal_rect(fb, fb_w, fb_h, y_origin, 194, 68, 62, 30, 0x8410);
    cal_rect(fb, fb_w, fb_h, y_origin, 196, 70, 58, 26, 0x0000);

    /* 16-step grey ramp */
    for (int i = 0; i < 16; i++) {
        const int v = (i * 255) / 15;
        uint16_t c = (uint16_t)(((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3));
        cal_rect(fb, fb_w, fb_h, y_origin, 8 + i * 19, 104, 19, 22, c);
    }

    /* R, G, B ramps (0 -> full) */
    for (int i = 0; i < 24; i++) {
        const int v = (i * 255) / 23;
        uint16_t r = (uint16_t)(((v >> 3) << 11));
        uint16_t g = (uint16_t)(((v >> 2) << 5));
        uint16_t b = (uint16_t)(v >> 3);
        cal_rect(fb, fb_w, fb_h, y_origin, 8 + i * 12, 132, 12, 10, r);
        cal_rect(fb, fb_w, fb_h, y_origin, 8 + i * 12, 144, 12, 10, g);
        cal_rect(fb, fb_w, fb_h, y_origin, 8 + i * 12, 156, 12, 10, b);
    }

    /* Near-black ladder: the violet cast is most visible here */
    static const uint16_t darks[8] = {0x0000, 0x0020, 0x0841, 0x0842,
                                      0x1000, 0x1800, 0x2000, 0x2945};
    for (int i = 0; i < 8; i++) {
        cal_rect(fb, fb_w, fb_h, y_origin, 8 + i * 38, 172, 38, 18, darks[i]);
    }

    /* Text samples: light on black and light on surface */
    ui_draw_text(fb, fb_w, fb_h, y_origin, 8, 12, "CAL 320x240",
                 michi_ui_font_get(MICHI_FONT_MD), 0xFFFF);
#ifdef CONFIG_MICHI_UI_CAL_FORCE_BIG_ENDIAN
    ui_draw_text(fb, fb_w, fb_h, y_origin, 210, 16, "BIG endian",
                 michi_ui_font_get(MICHI_FONT_SM), 0xFFE0);
#else
    ui_draw_text(fb, fb_w, fb_h, y_origin, 210, 16, "LITTLE endian",
                 michi_ui_font_get(MICHI_FONT_SM), 0x07E0);
#endif
    cal_rect(fb, fb_w, fb_h, y_origin, 8, 196, 304, 40, 0x0861);
    ui_draw_text(fb, fb_w, fb_h, y_origin, 16, 204, "Texto claro sobre superficie",
                 michi_ui_font_get(MICHI_FONT_SM), 0xF7BE);
    ui_draw_text(fb, fb_w, fb_h, y_origin, 16, 222, "Acentos: á é í ó ú ñ ¿? ¡!",
                 michi_ui_font_get(MICHI_FONT_SM), 0xFFFF);
}
#endif
