/* Host-side tests for the Michi UI design system components (UI-02).
 *
 * Compiles the REAL firmware sources - michi_ui_text.c, michi_ui_fonts.c
 * (with the generated flash tables) and michi_ui_components.c - no
 * reimplementation. Proves:
 *   - the wrap/ellipsize text contracts on the REAL proportional font
 *     metrics (SM/MD/PIN),
 *   - the MS-11 band contract: rendering every screen component into the
 *     8 band framebuffers (240 x 40) is pixel-identical to a full-frame
 *     (240 x 320) render, including elements straddling band boundaries,
 *   - component smoke behavior (chrome rows, dot radius, progress
 *     outline/fill),
 *   - the PIN ellipsis fallback: '…' renders as '.' (pin_map fix).
 *
 * Fake framebuffers are plain uint16_t arrays on the heap; no cJSON
 * dependency.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "michi_ui.h"

#define PANEL_W 240
#define PANEL_H 320
#define BAND_H 40
#define N_BANDS (PANEL_H / BAND_H)

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s\n", msg);                                     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

/* --------------------------------------------------------------------------
 * Wrap regression suite (UI-01 review follow-up): the REAL SM font
 * metrics from the generated tables (space=3, A=8, B=7, C=8, D=8, ...).
 * -------------------------------------------------------------------------- */

static void expect_wrap(const char *input, int max_w, int max_lines,
                        const char *const *expect, int expect_n)
{
    const michi_ui_font_t *sm = michi_ui_font_get(MICHI_FONT_SM);
    char buf[64];
    const char *lines[8];
    int n;
    int i;

    strcpy(buf, input);
    n = ui_wrap_text(sm, buf, max_w, lines, max_lines);
    if (n != expect_n) {
        printf("  FAIL wrap '%s'@%d: %d lines (want %d)\n", input, max_w,
               n, expect_n);
        failures++;
        return;
    }
    for (i = 0; i < n; i++) {
        if (strcmp(lines[i], expect[i]) != 0) {
            printf("  FAIL wrap '%s'@%d line %d: '%s' (want '%s')\n",
                   input, max_w, i, lines[i], expect[i]);
            failures++;
        }
    }
}

static void test_wrap(void)
{
    static const char *ab_c[] = { "AB", "C" };
    static const char *hello[] = { "hello", "world" };
    static const char *ab_c_d_efg[] = { "AB", "C D", "EFG" };
    static const char *ab[] = { "AB" };
    static const char *ab_cd[] = { "AB", "CD" };
    static const char *long_word[] = { "Averlongsingleword" };

    printf("michi_ui: wrap regression (real SM metrics)\n");
    expect_wrap("AB C", 10, 8, ab_c, 2);
    expect_wrap("hello world", 50, 8, hello, 2);
    expect_wrap("AB C D EFG", 20, 8, ab_c_d_efg, 3);
    expect_wrap("AB ", 10, 8, ab, 1); /* no empty line after the space */
    expect_wrap("AB \nCD", 10, 2, ab_cd, 2); /* newline after the break */
    expect_wrap("Averlongsingleword", 20, 8, long_word, 1); /* intact */
    expect_wrap("hello world", 50, 1, hello, 1); /* max_lines=1 */
}

/* --------------------------------------------------------------------------
 * Ellipsize (REAL MD metrics; "Very Long Title..." measures 102 > 100).
 * -------------------------------------------------------------------------- */

static void test_ellipsize(void)
{
    const michi_ui_font_t *md = michi_ui_font_get(MICHI_FONT_MD);
    char buf[64];
    char short_buf[32];
    size_t len;

    printf("michi_ui: ellipsize\n");
    strcpy(buf, "Very Long Title...");
    (void)ui_ellipsize(md, buf, 100);
    len = strlen(buf);
    CHECK(len >= 3, "ellipsized result has room for the marker");
    if (len >= 3) {
        CHECK((unsigned char)buf[len - 3] == 0xE2 &&
                  (unsigned char)buf[len - 2] == 0x80 &&
                  (unsigned char)buf[len - 1] == 0xA6,
              "ellipsized result ends with U+2026");
    }
    CHECK(ui_text_measure(md, buf) <= 100, "ellipsized result fits max_w");

    strcpy(short_buf, "Hi");
    (void)ui_ellipsize(md, short_buf, 100);
    CHECK(strcmp(short_buf, "Hi") == 0, "short string left unchanged");
}

/* --------------------------------------------------------------------------
 * Band identity: one component rendered into a full 240 x 320 frame must
 * equal the same component rendered band-by-band (240 x 40, y_origin =
 * 0, 40, ..., 280). Elements are placed so they straddle band boundaries.
 * -------------------------------------------------------------------------- */

typedef void (*draw_fn)(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                        uint16_t y_origin);

static void check_band_identity(const char *name, draw_fn draw)
{
    uint16_t *ref = malloc((size_t)PANEL_W * PANEL_H * sizeof(uint16_t));
    uint16_t *band = malloc((size_t)PANEL_W * BAND_H * sizeof(uint16_t));
    int b;

    CHECK(ref != NULL && band != NULL, "band identity: fb allocation");
    if (ref == NULL || band == NULL) {
        free(ref);
        free(band);
        return;
    }

    memset(ref, 0, (size_t)PANEL_W * PANEL_H * sizeof(uint16_t));
    draw(ref, PANEL_W, PANEL_H, 0);
    for (b = 0; b < N_BANDS; b++) {
        const uint16_t y_origin = (uint16_t)(b * BAND_H);

        memset(band, 0, (size_t)PANEL_W * BAND_H * sizeof(uint16_t));
        draw(band, PANEL_W, BAND_H, y_origin);
        if (memcmp(band, ref + (size_t)y_origin * PANEL_W,
                   (size_t)PANEL_W * BAND_H * sizeof(uint16_t)) != 0) {
            printf("  FAIL band identity %s: band %d differs from the "
                   "full-frame rows\n", name, b);
            failures++;
        }
    }
    free(ref);
    free(band);
}

static void draw_header(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                        uint16_t y_origin)
{
    michi_ui_draw_header(fb, fb_w, fb_h, y_origin, "Michi Music");
}

static void draw_footer(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                        uint16_t y_origin)
{
    michi_ui_draw_footer(fb, fb_w, fb_h, y_origin, "v1.2.3-build42");
}

/* cy=40: rows 37..43 straddle bands 0 and 1. */
static void draw_dot(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                     uint16_t y_origin)
{
    michi_ui_draw_status_dot(fb, fb_w, fb_h, y_origin, 60, 40,
                             MICHI_UI_ERROR);
}

/* rect y=30 h=60 (4 lines of SM): rows 30..89 span bands 0, 1, 2. */
static void draw_multiline(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                           uint16_t y_origin)
{
    const michi_ui_rect_t rect = { 20, 30, 200, 60 };

    (void)michi_ui_draw_multiline(fb, fb_w, fb_h, y_origin, &rect,
                                  MICHI_FONT_SM,
                                  "Michi Music streaming the world",
                                  MICHI_UI_ALIGN_CENTER,
                                  MICHI_UI_TEXT_SECONDARY);
}

/* Bar rows 35..44 straddle bands 0 and 1. */
static void draw_progress(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                          uint16_t y_origin)
{
    michi_ui_draw_progress(fb, fb_w, fb_h, y_origin, 10, 35, 220, 10, 60);
}

/* y_center=40 with an over-240 px string: ellipsized, straddles bands
 * 0 and 1. */
static void draw_message(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                         uint16_t y_origin)
{
    michi_ui_draw_centered_message(fb, fb_w, fb_h, y_origin, 40,
                                   MICHI_FONT_MD,
                                   "The quick brown fox jumps over the lazy "
                                   "dog while streaming hi-res audio",
                                   MICHI_UI_INFO);
}

static void test_band_identity(void)
{
    printf("michi_ui: band-vs-fullframe identity\n");
    check_band_identity("header", draw_header);
    check_band_identity("footer", draw_footer);
    check_band_identity("status dot (straddles band 0/1)", draw_dot);
    check_band_identity("multiline (spans bands 0-2)", draw_multiline);
    check_band_identity("progress (straddles band 0/1)", draw_progress);
    check_band_identity("centered message (straddles band 0/1)",
                        draw_message);
}

/* --------------------------------------------------------------------------
 * Component smoke: expected pixels near the chrome rows, dot radius and
 * progress outline/fill behavior.
 * -------------------------------------------------------------------------- */

static int count_color(const uint16_t *fb, int rows, uint16_t color)
{
    int n = 0;
    int i;

    for (i = 0; i < PANEL_W * rows; i++) {
        if (fb[i] == color) {
            n++;
        }
    }
    return n;
}

static void test_smoke(void)
{
    uint16_t *fb = malloc((size_t)PANEL_W * BAND_H * sizeof(uint16_t));

    printf("michi_ui: component smoke\n");
    CHECK(fb != NULL, "smoke: fb allocation");
    if (fb == NULL) {
        return;
    }

    /* Header: band 0 contains TEXT_PRIMARY pixels in rows 8..25 (MD). */
    memset(fb, 0, (size_t)PANEL_W * BAND_H * sizeof(uint16_t));
    michi_ui_draw_header(fb, PANEL_W, BAND_H, 0, "Michi Music");
    {
        int found = 0;
        int y;

        for (y = MICHI_UI_HEADER_Y; y < MICHI_UI_HEADER_Y + 18; y++) {
            if (count_color(fb + y * PANEL_W, 1, MICHI_UI_TEXT_PRIMARY) >
                0) {
                found = 1;
                break;
            }
        }
        CHECK(found, "header draws TEXT_PRIMARY pixels at its rows");
    }

    /* Footer: band 7 (y_origin 280) contains MUTED pixels at absolute
     * rows 304..318 (SM, local rows 24..38). */
    memset(fb, 0, (size_t)PANEL_W * BAND_H * sizeof(uint16_t));
    michi_ui_draw_footer(fb, PANEL_W, BAND_H, 280, "v1.2.3");
    {
        int found = 0;
        int y;

        for (y = MICHI_UI_FOOTER_Y - 280; y < BAND_H; y++) {
            if (count_color(fb + y * PANEL_W, 1, MICHI_UI_MUTED) > 0) {
                found = 1;
                break;
            }
        }
        CHECK(found, "footer draws MUTED pixels at its rows");
    }

    /* Status dot: radius 3 filled circle at (10, 10). */
    memset(fb, 0, (size_t)PANEL_W * BAND_H * sizeof(uint16_t));
    michi_ui_draw_status_dot(fb, PANEL_W, BAND_H, 0, 10, 10, MICHI_UI_ERROR);
    CHECK(fb[10 * PANEL_W + 10] == MICHI_UI_ERROR, "dot center filled");
    CHECK(fb[10 * PANEL_W + 13] == MICHI_UI_ERROR, "dot edge dx=3 filled");
    CHECK(fb[10 * PANEL_W + 14] == 0, "dot right outside r=3 untouched");
    CHECK(fb[6 * PANEL_W + 10] == 0, "dot top outside r=3 untouched");
    CHECK(fb[7 * PANEL_W + 10] == MICHI_UI_ERROR, "dot edge dy=-3 filled");

    /* Progress pct=0: muted outline only, no accent anywhere. */
    memset(fb, 0, (size_t)PANEL_W * BAND_H * sizeof(uint16_t));
    michi_ui_draw_progress(fb, PANEL_W, BAND_H, 0, 100, 10, 80, 12, 0);
    CHECK(fb[10 * PANEL_W + 100] == MICHI_UI_MUTED, "pct=0 outline top");
    CHECK(fb[10 * PANEL_W + 179] == MICHI_UI_MUTED, "pct=0 outline top-right");
    CHECK(fb[15 * PANEL_W + 140] == 0, "pct=0 interior untouched");
    CHECK(count_color(fb, BAND_H, MICHI_UI_ACCENT) == 0,
          "pct=0 draws no accent fill");

    /* Progress pct=100: interior fully accent, outline preserved. */
    memset(fb, 0, (size_t)PANEL_W * BAND_H * sizeof(uint16_t));
    michi_ui_draw_progress(fb, PANEL_W, BAND_H, 0, 100, 10, 80, 12, 100);
    CHECK(fb[15 * PANEL_W + 140] == MICHI_UI_ACCENT, "pct=100 interior filled");
    CHECK(fb[10 * PANEL_W + 100] == MICHI_UI_MUTED, "pct=100 keeps outline");

    /* Progress pct clamps: 200 renders like 100 (interior filled). */
    memset(fb, 0, (size_t)PANEL_W * BAND_H * sizeof(uint16_t));
    michi_ui_draw_progress(fb, PANEL_W, BAND_H, 0, 100, 10, 80, 12, 200);
    CHECK(fb[15 * PANEL_W + 140] == MICHI_UI_ACCENT, "pct>100 clamps to full");

    free(fb);
}

/* --------------------------------------------------------------------------
 * PIN ellipsis fallback: after the pin_map fix, '…' renders as the '.'
 * glyph (a truncated PIN shows a dot, not a blank).
 * -------------------------------------------------------------------------- */

static void test_pin_ellipsis(void)
{
    const michi_ui_font_t *pin = michi_ui_font_get(MICHI_FONT_PIN);
    /* pin_map is the generated michi_ui_pin_map table, exposed through
     * the font descriptor (same array the renderer reads). */
    const uint8_t *pin_map = pin->pin_map;
    uint16_t *a = malloc((size_t)PANEL_W * BAND_H * sizeof(uint16_t));
    uint16_t *b = malloc((size_t)PANEL_W * BAND_H * sizeof(uint16_t));
    char buf[64];
    size_t len;

    printf("michi_ui: PIN ellipsis fallback\n");
    CHECK(pin_map != NULL, "PIN font has a pin_map");
    CHECK(pin_map != NULL &&
              pin_map[MICHI_UI_FONT_ELLIPSIS_INDEX] == pin_map['.' - 0x20],
          "pin_map maps '…' to the '.' glyph");
    CHECK(pin_map != NULL &&
              pin_map[MICHI_UI_FONT_ELLIPSIS_INDEX] != pin_map[' ' - 0x20],
          "pin_map no longer maps '…' to space");

    /* Render proof: an ellipsized digit string must draw pixel-identical
     * to the same string with the '…' tail replaced by '.'. */
    if (a == NULL || b == NULL) {
        free(a);
        free(b);
        return;
    }
    strcpy(buf, "123456789012345678901234");
    (void)ui_ellipsize(pin, buf, 120);
    len = strlen(buf);
    CHECK(len >= 3, "PIN ellipsized result has the marker");
    if (len >= 3) {
        memset(a, 0, (size_t)PANEL_W * BAND_H * sizeof(uint16_t));
        ui_draw_text(a, PANEL_W, BAND_H, 0, 0, 0, buf, pin,
                     MICHI_UI_TEXT_PRIMARY);
        buf[len - 3] = '.';
        buf[len - 2] = '\0';
        memset(b, 0, (size_t)PANEL_W * BAND_H * sizeof(uint16_t));
        ui_draw_text(b, PANEL_W, BAND_H, 0, 0, 0, buf, pin,
                     MICHI_UI_TEXT_PRIMARY);
        CHECK(memcmp(a, b, (size_t)PANEL_W * BAND_H * sizeof(uint16_t)) ==
                  0,
              "ellipsized PIN renders a '.' as its last glyph");
    }
    free(a);
    free(b);
}

int main(void)
{
    test_wrap();
    test_ellipsize();
    test_band_identity();
    test_smoke();
    test_pin_ellipsis();
    if (failures == 0) {
        printf("PASS test_michi_ui\n");
        return 0;
    }
    printf("FAIL test_michi_ui (%d)\n", failures);
    return 1;
}
