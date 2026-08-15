#include <stddef.h>

#include "michi_ui_icons.h"

#include "../assets/icons/michi_ui_icons_data.h"

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
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_cat_20 },
        [MICHI_UI_ICON_SIZE_24] = { 24, 24, michi_ui_icon_cat_24 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_cat_32 },
        [MICHI_UI_ICON_SIZE_48] = { 48, 48, michi_ui_icon_cat_48 },
    },
    [MICHI_UI_ICON_WIFI] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_wifi_12 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_wifi_20 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_wifi_32 },
    },
    [MICHI_UI_ICON_SERVER] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_server_12 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_server_20 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_server_32 },
    },
    [MICHI_UI_ICON_SPEAKER] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_speaker_12 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_speaker_20 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_speaker_32 },
    },
    [MICHI_UI_ICON_PLAY] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_play_12 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_play_20 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_play_32 },
    },
    [MICHI_UI_ICON_PAUSE] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_pause_12 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_pause_20 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_pause_32 },
    },
    [MICHI_UI_ICON_PAIR] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_pair_12 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_pair_20 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_pair_32 },
    },
    [MICHI_UI_ICON_BUTTON] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_button_12 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_button_20 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_button_32 },
    },
    [MICHI_UI_ICON_WARNING] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_warning_12 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_warning_20 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_warning_32 },
    },
    [MICHI_UI_ICON_ERROR] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_error_12 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_error_20 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_error_32 },
    },
    [MICHI_UI_ICON_UPDATE] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_update_12 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_update_20 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_update_32 },
    },
    [MICHI_UI_ICON_WAVE] = {
        [MICHI_UI_ICON_SIZE_12] = { 12, 12, michi_ui_icon_wave_12 },
        [MICHI_UI_ICON_SIZE_20] = { 20, 20, michi_ui_icon_wave_20 },
        [MICHI_UI_ICON_SIZE_32] = { 32, 32, michi_ui_icon_wave_32 },
    },
};

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
