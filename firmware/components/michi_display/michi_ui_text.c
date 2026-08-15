#include <string.h>

#include "michi_ui.h"

#define MICHI_UI_ELLIPSIS_UTF8 "\xE2\x80\xA6"

int ui_text_measure(const michi_ui_font_t *font, const char *str)
{
    int w = 0;
    const char *p;

    if (font == NULL || str == NULL) {
        return 0;
    }
    p = str;
    while (*p != '\0') {
        w += michi_ui_font_advance(font, michi_ui_font_decode(font, &p));
    }
    return w;
}

int ui_wrap_text(const michi_ui_font_t *font, char *str, int max_w,
                 const char **out_lines, int max_lines)
{
    const char *line_start;
    const char *p;
    int line_w = 0;
    int count = 0;

    if (font == NULL || str == NULL || out_lines == NULL || max_lines <= 0 ||
        max_w <= 0) {
        return 0;
    }
    if (*str == '\0') {
        return 0;
    }

    line_start = str;
    p = str;

    for (;;) {
        const char *last_space = NULL; /* start of last space in this line */

        for (;;) {
            const char *glyph;
            int gw;
            uint8_t idx;

            if (*p == '\0' || *p == '\n') {
                int is_newline = (*p == '\n');
                *(char *)p = '\0';
                out_lines[count++] = line_start;
                if (is_newline) {
                    p++;
                }
                line_start = p;
                line_w = 0;
                break;
            }

            glyph = p;
            idx = michi_ui_font_decode(font, &p);
            gw = michi_ui_font_advance(font, idx);

            if (line_w + gw > max_w && line_w > 0) {
                /* The glyph does not fit on this line. */
                if (*glyph == ' ') {
                    /* H4 (UI-01 review fix): the line is full and the
                     * overflow glyph is a space. End the current line at
                     * that space and start the next one right after it.
                     * Hard-cutting a space was the original bug: it
                     * emitted an empty next line and dropped the
                     * following word. */
                    *(char *)glyph = '\0';
                    if (line_start != glyph) {
                        out_lines[count++] = line_start;
                        if (count >= max_lines) {
                            return count; /* remaining text dropped */
                        }
                    }
                    line_start = glyph + 1;
                    line_w = 0;
                    last_space = NULL; /* stale boundary before the new
                                        * line start must never fire */
                    continue;
                }
                if (last_space != NULL) {
                    /* Break at the last word boundary. */
                    *(char *)last_space = '\0';
                    if (line_start != last_space) {
                        out_lines[count++] = line_start;
                        if (count >= max_lines) {
                            return count; /* remaining text dropped */
                        }
                    }
                    line_start = last_space + 1;
                    last_space = NULL;
                    /* Re-measure the new line from its start (bounded
                     * by the remaining text). */
                    line_w = 0;
                    const char *q = line_start;
                    while (q < glyph) {
                        line_w += michi_ui_font_advance(
                            font, michi_ui_font_decode(font, &q));
                    }
                }
                /* else: an unbreakable word wider than the line is kept
                 * intact and breaks at the next space/NUL (the caller
                 * ellipsizes long words via ui_ellipsize when a hard
                 * bound is required - cutting mid-word here would lose
                 * the word entirely). */
            }

            line_w += gw;
            if (*glyph == ' ') {
                last_space = glyph;
            }
        }

        if (count >= max_lines || *line_start == '\0') {
            return count;
        }
    }
}

char *ui_ellipsize(const michi_ui_font_t *font, char *str, int max_w)
{
    const char *ell = MICHI_UI_ELLIPSIS_UTF8;
    int ell_w;
    int budget;
    const char *p;
    const char *cut;   /* byte position after the last fitting glyph */
    const char *word;  /* byte position after the last space (or NULL) */
    int w = 0;
    int cut_w = 0;
    int word_w = 0;
    size_t cut_len;

    if (font == NULL || str == NULL || *str == '\0') {
        return str;
    }
    if (ui_text_measure(font, str) <= max_w) {
        return str;
    }

    ell_w = ui_text_measure(font, ell);
    if (ell_w >= max_w) {
        /* Even the ellipsis does not fit: hard-truncate, no marker. */
        budget = max_w;
        ell = "";
        ell_w = 0;
    } else {
        budget = max_w - ell_w;
    }

    cut = str;
    word = NULL;
    p = str;
    while (*p != '\0') {
        const char *glyph = p;
        uint8_t idx = michi_ui_font_decode(font, &p);
        int gw = michi_ui_font_advance(font, idx);

        if (w + gw > budget) {
            break;
        }
        w += gw;
        cut = p;
        cut_w = w;
        if (*glyph == ' ') {
            word = p;
            word_w = w;
        }
    }

    /* Prefer the last word boundary when it keeps at least half of the
     * pixel cut: avoids a mid-word cut "when avoidable". The boundary is
     * cut AT the space, so the result ends with a full word (no trailing
     * space before the ellipsis). */
    if (word != NULL && word_w * 2 >= cut_w) {
        cut = word - 1;
    }

    cut_len = (size_t)(cut - str);
    str[cut_len] = '\0';
    if (ell[0] != '\0') {
        memcpy(str + cut_len, ell, 4); /* ellipsis + NUL */
    }
    return str;
}

static void draw_text_internal(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                               uint16_t y_origin, int x, int y,
                               const char *str, const michi_ui_font_t *font,
                               uint16_t color)
{
    const char *p;
    int line_h;
    int ly;

    if (font == NULL || str == NULL || *str == '\0') {
        return;
    }
    line_h = michi_ui_font_line_height(font);
    ly = y - (int)y_origin;
    if (ly + line_h <= 0 || ly >= (int)fb_h) {
        return; /* the whole row does not intersect this band */
    }

    p = str;
    while (*p != '\0') {
        uint8_t idx;

        if (x >= (int)fb_w) {
            return; /* everything after this glyph is off-band right */
        }
        idx = michi_ui_font_decode(font, &p);
        michi_ui_font_draw_glyph(fb, fb_w, fb_h, x, ly, font, idx, color);
        x += michi_ui_font_advance(font, idx);
    }
}

void ui_draw_text(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                  uint16_t y_origin, int x, int y, const char *str,
                  const michi_ui_font_t *font, uint16_t color)
{
    draw_text_internal(fb, fb_w, fb_h, y_origin, x, y, str, font, color);
}

void ui_draw_text_centered(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                           uint16_t y_origin, int y, const char *str,
                           const michi_ui_font_t *font, uint16_t color)
{
    int w;
    int x;

    if (font == NULL || str == NULL) {
        return;
    }
    w = ui_text_measure(font, str);
    x = ((int)fb_w - w) / 2;
    if (x < 0) {
        x = 0; /* overlong strings render left-clipped instead of negative */
    }
    draw_text_internal(fb, fb_w, fb_h, y_origin, x, y, str, font, color);
}

void ui_draw_text_aligned(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                          uint16_t y_origin, const michi_ui_rect_t *box,
                          int y, const char *str,
                          const michi_ui_font_t *font, uint16_t color,
                          michi_ui_align_t align)
{
    int w;
    int x;

    if (box == NULL || font == NULL || str == NULL) {
        return;
    }
    w = ui_text_measure(font, str);
    switch (align) {
    case MICHI_UI_ALIGN_RIGHT:
        x = box->x + box->w - w;
        break;
    case MICHI_UI_ALIGN_CENTER:
        x = box->x + (box->w - w) / 2;
        break;
    case MICHI_UI_ALIGN_LEFT:
    default:
        x = box->x;
        break;
    }
    draw_text_internal(fb, fb_w, fb_h, y_origin, x, y, str, font, color);
}

void ui_draw_multiline(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                       uint16_t y_origin, const char *const *lines,
                       int count, int x, int y, int line_h,
                       const michi_ui_font_t *font, uint16_t color)
{
    int i;

    if (lines == NULL || font == NULL || count <= 0) {
        return;
    }
    for (i = 0; i < count; i++) {
        draw_text_internal(fb, fb_w, fb_h, y_origin, x, y, lines[i], font,
                           color);
        y += line_h;
    }
}
