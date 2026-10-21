#include <stddef.h>

#include "michi_ui_icons.h"

#include "../assets/icons/michi_ui_icons_data.h"
#include "../assets/icons/michi_stream_brand_data.h"
#include "../assets/icons/michi_precision_icons_data.h"

/*
 * Icon glyph registry: (icon, size) -> bitmap table. Every bitmap is
 * generated static const flash data; NULL marks sizes that are not
 * generated for a given icon (the cat is the only icon with 24/48).
 */
typedef struct michi_ui_icon_glyph {
    uint8_t w;
    uint8_t h;
    const uint8_t *data; /* row-major 1 bpp MSB-first */
} michi_ui_icon_glyph_t;

static const michi_ui_icon_glyph_t s_icons[MICHI_UI_ICON_COUNT]
                                          [MICHI_UI_ICON_SIZE_COUNT] = {
    [MICHI_UI_ICON_CAT] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_cat_12 },
        [MICHI_UI_ICON_SIZE_16] = { 16, 16, michi_ui_icon_cat_16 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_cat_20 },
        [MICHI_UI_ICON_SIZE_24] = { 24, 24, michi_ui_icon_cat_24 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_cat_32 },
        [MICHI_UI_ICON_SIZE_48] = { 48, 48, michi_ui_icon_cat_48 },
    },
    [MICHI_UI_ICON_WIFI] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_wifi_12 },
        [MICHI_UI_ICON_SIZE_16] = { 16, 16, michi_ui_icon_wifi_16 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_wifi_20 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_wifi_32 },
    },
    [MICHI_UI_ICON_SERVER] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_server_12 },
        [MICHI_UI_ICON_SIZE_16] = { 16, 16, michi_ui_icon_server_16 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_server_20 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_server_32 },
    },
    [MICHI_UI_ICON_SPEAKER] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_speaker_12 },
        [MICHI_UI_ICON_SIZE_16] = { 16, 16, michi_ui_icon_speaker_16 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_speaker_20 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_speaker_32 },
    },
    [MICHI_UI_ICON_PLAY] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_play_12 },
        [MICHI_UI_ICON_SIZE_16] = { 16, 16, michi_ui_icon_play_16 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_play_20 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_play_32 },
    },
    [MICHI_UI_ICON_PAUSE] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_pause_12 },
        [MICHI_UI_ICON_SIZE_16] = { 16, 16, michi_ui_icon_pause_16 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_pause_20 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_pause_32 },
    },
    [MICHI_UI_ICON_PAIR] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_pair_12 },
        [MICHI_UI_ICON_SIZE_16] = { 16, 16, michi_ui_icon_pair_16 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_pair_20 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_pair_32 },
    },
    [MICHI_UI_ICON_BUTTON] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_button_12 },
        [MICHI_UI_ICON_SIZE_16] = { 16, 16, michi_ui_icon_button_16 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_button_20 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_button_32 },
    },
    [MICHI_UI_ICON_WARNING] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_warning_12 },
        [MICHI_UI_ICON_SIZE_16] = { 16, 16, michi_ui_icon_warning_16 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_warning_20 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_warning_32 },
    },
    [MICHI_UI_ICON_ERROR] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_error_12 },
        [MICHI_UI_ICON_SIZE_16] = { 16, 16, michi_ui_icon_error_16 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_error_20 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_error_32 },
    },
    [MICHI_UI_ICON_UPDATE] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_update_12 },
        [MICHI_UI_ICON_SIZE_16] = { 16, 16, michi_ui_icon_update_16 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_update_20 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_update_32 },
    },
    [MICHI_UI_ICON_WAVE] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_wave_12 },
        [MICHI_UI_ICON_SIZE_16] = { 16, 16, michi_ui_icon_wave_16 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_wave_20 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_wave_32 },
    },
};

/* UI-06: optical 2bpp precision glyphs. Small static masks live in flash.
 * The RGB565 blend is bounded by the active DMA band; no heap or SVG decoder. */
static void draw_precision_icon(uint16_t *fb, uint16_t fw, uint16_t fh,
                                uint16_t oy, int x, int y,
                                const uint8_t *mask, int px, uint16_t fg)
{
    if (fb == NULL || mask == NULL || fw == 0 || fh == 0) return;
    if (y + px <= (int)oy || y >= (int)oy + (int)fh ||
        x + px <= 0 || x >= (int)fw) return;
    for (int py = 0; py < px; ++py) {
        int yy = y + py - (int)oy;
        if (yy < 0 || yy >= (int)fh) continue;
        for (int pxl = 0; pxl < px; ++pxl) {
            int xx = x + pxl;
            if (xx < 0 || xx >= (int)fw) continue;
            unsigned index = (unsigned)py * (unsigned)px + (unsigned)pxl;
            unsigned alpha = (mask[index / 4u] >> (6u - 2u*(index & 3u))) & 3u;
            if (alpha == 0u) continue;
            uint16_t *pixel = &fb[(size_t)yy * fw + (size_t)xx];
            if (alpha == 3u) { *pixel = fg; continue; }
            uint16_t bg = *pixel;
            unsigned r = ((((bg >> 11) & 31u) * (3u-alpha)) +
                          (((fg >> 11) & 31u) * alpha) + 1u) / 3u;
            unsigned g = ((((bg >> 5) & 63u) * (3u-alpha)) +
                          (((fg >> 5) & 63u) * alpha) + 1u) / 3u;
            unsigned b = (((bg & 31u) * (3u-alpha)) +
                          ((fg & 31u) * alpha) + 1u) / 3u;
            *pixel = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

static const uint8_t *precision_icon(michi_ui_icon_t icon,
                                      michi_ui_icon_size_t size, int *pixels)
{
    int px;
    if (size == MICHI_UI_ICON_SIZE_12) px=12;
    else if (size == MICHI_UI_ICON_SIZE_16) px=16;
    else if (size == MICHI_UI_ICON_SIZE_20) px=20;
    else if (size == MICHI_UI_ICON_SIZE_32) px=32;
    else return NULL;
    *pixels=px;
    switch (icon) {
    case MICHI_UI_ICON_WIFI:
        switch(px) { case 12:return michi_precision_wifi_12; case 16:return michi_precision_wifi_16;
                     case 20:return michi_precision_wifi_20; default:return michi_precision_wifi_32; }
    case MICHI_UI_ICON_SERVER:
        switch(px) { case 12:return michi_precision_receiver_12; case 16:return michi_precision_receiver_16;
                     case 20:return michi_precision_receiver_20; default:return michi_precision_receiver_32; }
    case MICHI_UI_ICON_ROOM:
        switch(px) { case 12:return michi_precision_room_12; case 16:return michi_precision_room_16;
                     case 20:return michi_precision_room_20; default:return michi_precision_room_32; }
    case MICHI_UI_ICON_SPEAKER:
        switch(px) { case 12:return michi_precision_speaker_12; case 16:return michi_precision_speaker_16;
                     case 20:return michi_precision_speaker_20; default:return michi_precision_speaker_32; }
    default:return NULL;
    }
}

void ui_draw_icon(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                  uint16_t y_origin, int x, int y, michi_ui_icon_t icon,
                  michi_ui_icon_size_t size, uint16_t color)
{
    const michi_ui_icon_glyph_t *g;
    const uint8_t *d;
    uint8_t row;
    uint8_t row_bytes;
    int ly;

    if (icon >= MICHI_UI_ICON_COUNT || size >= MICHI_UI_ICON_SIZE_COUNT) {
        return;
    }
    int optical_px = 0;
    const uint8_t *optical = precision_icon(icon, size, &optical_px);
    if (optical != NULL) {
        draw_precision_icon(fb, fb_w, fb_h, y_origin, x, y,
                            optical, optical_px, color);
        return;
    }
    g = &s_icons[icon][size];
    if (g->data == NULL) {
        return; /* not generated for this size */
    }

    ly = y - (int)y_origin;
    if (ly + (int)g->h <= 0 || ly >= (int)fb_h) {
        return; /* icon rows do not intersect this band */
    }

    d = g->data + 2;
    row_bytes = (uint8_t)((g->w + 7u) / 8u);
    for (row = 0; row < g->h; row++) {
        int py = ly + row;
        uint8_t col;

        if (py < 0 || py >= (int)fb_h) {
            continue;
        }
        for (col = 0; col < row_bytes; col++) {
            uint8_t bits = d[(size_t)row * row_bytes + col];
            uint8_t bit;

            for (bit = 0; bit < 8; bit++) {
                int px;

                if ((bits & (uint8_t)(0x80u >> bit)) == 0) {
                    continue;
                }
                px = x + (int)col * 8 + (int)bit;
                if (px < 0 || px >= (int)fb_w) {
                    continue;
                }
                fb[(size_t)py * fb_w + (size_t)px] = color;
            }
        }
    }
}

/* Official SVG baked to RGB565, no heap, alpha, blur or SVG on the MCU.
 * Only touch scanlines within the current 40px DMA band. */
void michi_ui_draw_brand(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                         uint16_t y_origin, int x, int y, int px_size)
{
    const uint16_t *pixels = NULL;
    if (fb == NULL || fb_w == 0 || fb_h == 0) return;
    if (px_size == 40) pixels = michi_brand_40;
    else if (px_size == 72) pixels = michi_brand_72;
    else return;
    for (int row = 0; row < px_size; ++row) {
        const int local_y = y + row - (int)y_origin;
        if (local_y < 0 || local_y >= (int)fb_h) continue;
        const int first = (x < 0) ? -x : 0;
        const int last = (x + px_size > (int)fb_w) ? (int)fb_w - x : px_size;
        if (last <= first) continue;
        for (int col = first; col < last; ++col) {
            fb[(size_t)local_y * fb_w + (size_t)(x + col)] = pixels[row * px_size + col];
        }
    }
}
