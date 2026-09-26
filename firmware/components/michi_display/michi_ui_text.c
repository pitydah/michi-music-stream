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

size_t michi_ui_utf8_safe_copy(char *dst, size_t dst_cap, const char *src)
{
    if (dst == NULL || dst_cap == 0) {
        return 0;
    }
    if (src == NULL || *src == '\0') {
        dst[0] = '\0';
        return 0;
    }

    size_t src_len = strlen(src);
    size_t max_copy = dst_cap - 1;
    if (src_len <= max_copy) {
        memcpy(dst, src, src_len);
        dst[src_len] = '\0';
        return src_len;
    }

    /* We must truncate to <= max_copy, without splitting a multi-byte UTF-8 codepoint. */
    size_t len = max_copy;
    while (len > 0 && ((unsigned char)src[len - 1] & 0xC0) == 0x80) {
        /* Move before continuation bytes */
        len--;
    }
    if (len > 0 && ((unsigned char)src[len - 1] & 0x80) != 0) {
        unsigned char lead = (unsigned char)src[len - 1];
        size_t req = 1;
        if ((lead & 0xE0) == 0xC0) {
            req = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            req = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            req = 4;
        }

        if (max_copy - (len - 1) < req) {
            /* Sequence was truncated: drop the incomplete leading byte */
            len = len - 1;
        } else {
            len = max_copy;
        }
    } else {
        len = max_copy;
    }

    memcpy(dst, src, len);
    dst[len] = '\0';
    return len;
}

int ui_wrap_text_ex(const michi_ui_font_t *font, char *str, int max_w,
                    const char **out_lines, int max_lines, bool *was_truncated)
{
    const char *line_start;
    const char *p;
    int line_w = 0;
    int count = 0;
    bool truncated = false;

    if (was_truncated != NULL) {
        *was_truncated = false;
    }

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
                if (*line_start != '\0') {
                    char *end = (char *)p - 1;
                    while (end >= line_start && *end == ' ') {
                        *end = '\0';
                        end--;
                    }
                    if (*line_start != '\0') {
                        out_lines[count++] = line_start;
                    }
                }
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
                    *(char *)glyph = '\0';
                    if (line_start != glyph) {
                        out_lines[count++] = line_start;
                        if (count >= max_lines) {
                            /* Check if there is remaining non-whitespace text */
                            const char *rem = glyph + 1;
                            while (*rem == ' ') rem++;
                            if (*rem != '\0') {
                                truncated = true;
                            }
                            goto finish;
                        }
                    }
                    line_start = glyph + 1;
                    line_w = 0;
                    last_space = NULL;
                    continue;
                }
                if (last_space != NULL) {
                    /* Break at the last word boundary. */
                    *(char *)last_space = '\0';
                    if (line_start != last_space) {
                        out_lines[count++] = line_start;
                        if (count >= max_lines) {
                            /* Remaining text dropped */
                            truncated = true;
                            goto finish;
                        }
                    }
                    line_start = last_space + 1;
                    last_space = NULL;
                    line_w = 0;
                    const char *q = line_start;
                    while (q < glyph) {
                        line_w += michi_ui_font_advance(
                            font, michi_ui_font_decode(font, &q));
                    }
                }
            }

            line_w += gw;
            if (*glyph == ' ') {
                last_space = glyph;
            }
        }

        if (count >= max_lines || *line_start == '\0') {
            if (count >= max_lines && *line_start != '\0') {
                const char *rem = line_start;
                while (*rem == ' ') rem++;
                if (*rem != '\0') {
                    truncated = true;
                }
            }
            break;
        }
    }

finish:
    if (truncated && count > 0) {
        char *last = (char *)out_lines[count - 1];
        const char *ell = MICHI_UI_ELLIPSIS_UTF8;
        int ell_w = ui_text_measure(font, ell);
        int last_w = ui_text_measure(font, last);
        if (last_w + ell_w <= max_w) {
            size_t len = strlen(last);
            memcpy(last + len, ell, 4);
        } else {
            int budget = max_w - ell_w;
            if (budget > 0) {
                int w = 0;
                const char *p = last;
                const char *cut = last;
                while (*p != '\0') {
                    const char *next = p;
                    uint8_t idx = michi_ui_font_decode(font, &next);
                    int gw = michi_ui_font_advance(font, idx);
                    if (w + gw > budget) {
                        break;
                    }
                    w += gw;
                    cut = next;
                    p = next;
                }
                size_t cut_len = (size_t)(cut - last);
                while (cut_len > 0 && last[cut_len - 1] == ' ') {
                    cut_len--;
                }
                last[cut_len] = '\0';
                memcpy(last + cut_len, ell, 4);
            }
        }
    }
    if (was_truncated != NULL) {
        *was_truncated = truncated;
    }
    return count;
}

int ui_wrap_text(const michi_ui_font_t *font, char *str, int max_w,
                 const char **out_lines, int max_lines)
{
    return ui_wrap_text_ex(font, str, max_w, out_lines, max_lines, NULL);
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
