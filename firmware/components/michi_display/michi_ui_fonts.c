#include <stddef.h>

#include "michi_ui_fonts.h"

#include "../assets/fonts/michi_ui_fonts_data.h"

/*
 * Font registry. Every descriptor points into the generated flash tables
 * (static const, no runtime allocation). The pin font remaps the full
 * charset onto its digits+space+dot subset via pin_map.
 */
static const michi_ui_font_t s_fonts[MICHI_FONT_COUNT] = {
    [MICHI_FONT_XS] = {
        "xs", 8, 6, MICHI_UI_FONT_GLYPH_COUNT,
        michi_ui_xs_bitmap, michi_ui_xs_width, michi_ui_xs_advance,
        michi_ui_xs_offset, NULL,
    },
    [MICHI_FONT_SM] = {
        "sm", 15, 11, MICHI_UI_FONT_GLYPH_COUNT,
        michi_ui_sm_bitmap, michi_ui_sm_width, michi_ui_sm_advance,
        michi_ui_sm_offset, NULL,
    },
    [MICHI_FONT_MD] = {
        "md", 18, 14, MICHI_UI_FONT_GLYPH_COUNT,
        michi_ui_md_bitmap, michi_ui_md_width, michi_ui_md_advance,
        michi_ui_md_offset, NULL,
    },
    [MICHI_FONT_LG] = {
        "lg", 26, 20, MICHI_UI_FONT_GLYPH_COUNT,
        michi_ui_lg_bitmap, michi_ui_lg_width, michi_ui_lg_advance,
        michi_ui_lg_offset, NULL,
    },
    [MICHI_FONT_PIN] = {
        "pin", 35, 35, MICHI_UI_FONT_GLYPH_COUNT,
        michi_ui_pin_bitmap, michi_ui_pin_width, michi_ui_pin_advance,
        michi_ui_pin_offset, michi_ui_pin_map,
    },
};

const michi_ui_font_t *michi_ui_font_get(michi_ui_font_id_t id)
{
    if (id >= MICHI_FONT_COUNT) {
        id = MICHI_FONT_XS;
    }
    return &s_fonts[id];
}

uint8_t michi_ui_font_decode(const michi_ui_font_t *font, const char **cursor)
{
    (void)font;
    const unsigned char *p = (const unsigned char *)*cursor;

    if (p[0] == 0xE2u && p[1] == 0x80u && p[2] == 0xA6u) {
        *cursor += 3;
        return MICHI_UI_FONT_ELLIPSIS_INDEX;
    }
    *cursor += 1;
    if (p[0] >= 0x20u && p[0] <= 0x7Eu) {
        return (uint8_t)(p[0] - 0x20u);
    }
    if (p[0] < 0x20u) {
        return 0; /* control character renders as space */
    }
    return 63; /* unknown byte renders as '?' */
}

int michi_ui_font_advance(const michi_ui_font_t *font, uint8_t full_idx)
{
    uint8_t idx = full_idx;
    if (font->pin_map != NULL) {
        idx = font->pin_map[full_idx];
    }
    return font->advance[idx];
}

void michi_ui_font_draw_glyph(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                              int x, int y, const michi_ui_font_t *font,
                              uint8_t full_idx, uint16_t color)
{
    uint8_t idx = full_idx;
    uint8_t bpc;
    uint8_t w;
    uint8_t col;

    if (font->pin_map != NULL) {
        idx = font->pin_map[full_idx];
    }
    if (y + (int)font->height <= 0 || y >= (int)fb_h) {
        return; /* glyph rows do not intersect this band */
    }

    bpc = (uint8_t)((font->height + 7u) / 8u);
    w = font->width[idx];

    for (col = 0; col < w; col++) {
        uint16_t base = font->offset[idx] + (uint16_t)col * bpc;
        int px = x + col;
        uint8_t b;

        if (px < 0 || px >= (int)fb_w) {
            continue;
        }
        for (b = 0; b < bpc; b++) {
            uint8_t bits = font->bitmap[base + b];
            uint8_t bit;

            for (bit = 0; bit < 8; bit++) {
                int py;
                int row;

                if ((bits & (uint8_t)(1u << bit)) == 0) {
                    continue;
                }
                row = (int)b * 8 + (int)bit;
                if (row >= (int)font->height) {
                    break; /* higher bits are also out of the em box */
                }
                py = y + row;
                if (py >= 0 && py < (int)fb_h) {
                    fb[(size_t)py * fb_w + (size_t)px] = color;
                }
            }
        }
    }
}
