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

/* --------------------------------------------------------------------------
 * Screen scenarios (UI-01..UI-15) + Full-frame vs Banded identity
 * -------------------------------------------------------------------------- */

typedef struct ui_scenario {
    const char *id;
    const char *description;
    michi_ui_screen_ctx_t ctx;
} ui_scenario_t;

static void check_screen_band_identity(const ui_scenario_t *sc)
{
    uint16_t *ref = malloc((size_t)PANEL_W * PANEL_H * sizeof(uint16_t));
    uint16_t *band = malloc((size_t)PANEL_W * BAND_H * sizeof(uint16_t));
    int b;
    int non_zero_pixels = 0;

    CHECK(ref != NULL && band != NULL, "scenario fb allocation");
    if (ref == NULL || band == NULL) {
        free(ref);
        free(band);
        return;
    }

    /* Render full frame (240 x 320) */
    memset(ref, 0, (size_t)PANEL_W * PANEL_H * sizeof(uint16_t));
    michi_ui_render_screen(ref, PANEL_W, PANEL_H, 0, &sc->ctx);

    /* Verify non-empty screen (real graphics rendered) */
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        if (ref[i] != 0) {
            non_zero_pixels++;
        }
    }
    if (non_zero_pixels < 50) {
        printf("  FAIL %s (%s): too few visible pixels (%d)\n",
               sc->id, sc->description, non_zero_pixels);
        failures++;
    }

    /* Render band-by-band and verify exact bitwise identity */
    for (b = 0; b < N_BANDS; b++) {
        const uint16_t y_origin = (uint16_t)(b * BAND_H);

        memset(band, 0, (size_t)PANEL_W * BAND_H * sizeof(uint16_t));
        michi_ui_render_screen(band, PANEL_W, BAND_H, y_origin, &sc->ctx);

        if (memcmp(band, ref + (size_t)y_origin * PANEL_W,
                   (size_t)PANEL_W * BAND_H * sizeof(uint16_t)) != 0) {
            printf("  FAIL band identity %s (%s): band %d differs from full-frame\n",
                   sc->id, sc->description, b);
            failures++;
        }
    }

    free(ref);
    free(band);
}

static void test_all_screen_scenarios(void)
{
    printf("michi_ui: all 15 screen scenarios + band identity (UI-01..UI-15)\n");

    const ui_scenario_t scenarios[] = {
        {
            .id = "UI-01",
            .description = "boot",
            .ctx = { .state = MICHI_STATE_BOOTING }
        },
        {
            .id = "UI-02",
            .description = "unprovisioned",
            .ctx = { .state = MICHI_STATE_UNPROVISIONED }
        },
        {
            .id = "UI-03",
            .description = "ready",
            .ctx = { .state = MICHI_STATE_IDLE, .wifi_connected = true }
        },
        {
            .id = "UI-04",
            .description = "connecting",
            .ctx = { .state = MICHI_STATE_WIFI_CONNECTING }
        },
        {
            .id = "UI-05",
            .description = "pairing no PIN",
            .ctx = { .state = MICHI_STATE_PAIRING, .pairing_pin = NULL }
        },
        {
            .id = "UI-06",
            .description = "pairing PIN 123456",
            .ctx = { .state = MICHI_STATE_PAIRING, .pairing_pin = "123456" }
        },
        {
            .id = "UI-07",
            .description = "playing short title",
            .ctx = {
                .state = MICHI_STATE_PLAYING,
                .title = "Time",
                .artist = "Pink Floyd",
                .source = "Living Room",
                .volume = 72,
                .sample_rate = 48000,
                .bit_depth = 16,
                .wifi_connected = true,
                .server_connected = true,
            }
        },
        {
            .id = "UI-08",
            .description = "playing long title",
            .ctx = {
                .state = MICHI_STATE_PLAYING,
                .title = "Everybody Wants to Rule the World (Extended Version Remastered)",
                .artist = "Tears for Fears",
                .source = "Michi Micro Server",
                .volume = 72,
                .sample_rate = 48000,
                .bit_depth = 16,
            }
        },
        {
            .id = "UI-09",
            .description = "playing no metadata",
            .ctx = {
                .state = MICHI_STATE_PLAYING,
                .title = NULL,
                .artist = NULL,
                .source = NULL,
                .volume = 72,
                .sample_rate = 48000,
                .bit_depth = 16,
            }
        },
        {
            .id = "UI-10",
            .description = "paused",
            .ctx = {
                .state = MICHI_STATE_PAUSED,
                .title = "Time",
                .artist = "Pink Floyd",
                .source = "Living Room",
                .volume = 72,
                .sample_rate = 48000,
                .bit_depth = 16,
            }
        },
        {
            .id = "UI-11",
            .description = "buffering",
            .ctx = {
                .state = MICHI_STATE_BUFFERING,
                .title = "Time",
                .artist = "Pink Floyd",
                .volume = 72,
            }
        },
        {
            .id = "UI-12",
            .description = "updating",
            .ctx = {
                .state = MICHI_STATE_UPDATING,
                .update_pct = 68,
            }
        },
        {
            .id = "UI-13",
            .description = "recoverable error",
            .ctx = {
                .state = MICHI_STATE_RECOVERABLE_ERROR,
                .last_error = 0x3001,
            }
        },
        {
            .id = "UI-14",
            .description = "fatal error",
            .ctx = {
                .state = MICHI_STATE_FATAL_ERROR,
                .last_error = 0x101,
            }
        },
        {
            .id = "UI-15",
            .description = "diagnostics",
            .ctx = {
                .state = MICHI_STATE_IDLE,
                .show_diagnostics = true,
                .wifi_connected = true,
                .server_connected = true,
                .volume = 72,
                .sample_rate = 48000,
                .bit_depth = 16,
            }
        },
    };

    const size_t count = sizeof(scenarios) / sizeof(scenarios[0]);
    for (size_t i = 0; i < count; i++) {
        check_screen_band_identity(&scenarios[i]);
    }
}

/* --------------------------------------------------------------------------
 * Recoverable error contextual copy: an AUDIO error (0x103) must draw
 * "Recuperando audio" while a NETWORK error (0x3001) draws "Reconectando".
 * The two frames must differ in the title (and hint) zone and be
 * pixel-identical everywhere else (header, warning icon, chrome).
 * -------------------------------------------------------------------------- */

/* Row-range comparison helpers over full 240x320 frames. */
static int rows_equal(const uint16_t *a, const uint16_t *b, int y0, int y1)
{
    for (int y = y0; y < y1; y++) {
        if (memcmp(a + (size_t)y * PANEL_W, b + (size_t)y * PANEL_W,
                   (size_t)PANEL_W * sizeof(uint16_t)) != 0) {
            return 0;
        }
    }
    return 1;
}

static int rows_any_pixel(const uint16_t *fb, int y0, int y1)
{
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < PANEL_W; x++) {
            if (fb[(size_t)y * PANEL_W + x] != 0) {
                return 1;
            }
        }
    }
    return 0;
}

static void test_recoverable_error_contextual(void)
{
    printf("michi_ui: recoverable error contextual copy (audio vs network)\n");

    const michi_ui_screen_ctx_t audio_ctx = {
        .state = MICHI_STATE_RECOVERABLE_ERROR,
        .last_error = 0x103,
    };
    const michi_ui_screen_ctx_t net_ctx = {
        .state = MICHI_STATE_RECOVERABLE_ERROR,
        .last_error = 0x3001,
    };

    uint16_t *audio = malloc((size_t)PANEL_W * PANEL_H * sizeof(uint16_t));
    uint16_t *net = malloc((size_t)PANEL_W * PANEL_H * sizeof(uint16_t));
    uint16_t *ref = malloc((size_t)PANEL_W * PANEL_H * sizeof(uint16_t));
    CHECK(audio != NULL && net != NULL && ref != NULL,
          "recoverable contextual: fb allocation");
    if (audio == NULL || net == NULL || ref == NULL) {
        free(audio);
        free(net);
        free(ref);
        return;
    }

    memset(audio, 0, (size_t)PANEL_W * PANEL_H * sizeof(uint16_t));
    michi_ui_render_screen(audio, PANEL_W, PANEL_H, 0, &audio_ctx);
    memset(net, 0, (size_t)PANEL_W * PANEL_H * sizeof(uint16_t));
    michi_ui_render_screen(net, PANEL_W, PANEL_H, 0, &net_ctx);

    /* Title zone: rows 105..150 (LG text centered at y=125, descenders of
     * "Recuperando audio" reach row ~148). Hint zone: rows 160..201
     * (hint_rect y=160 h=40). */
    CHECK(!rows_equal(audio, net, 105, 150),
          "recoverable: audio vs network differ in the title zone");
    CHECK(!rows_equal(audio, net, 160, 201),
          "recoverable: audio vs network differ in the hint zone");
    CHECK(rows_equal(audio, net, 0, 105) &&
              rows_equal(audio, net, 150, 160) &&
              rows_equal(audio, net, 201, PANEL_H),
          "recoverable: frames identical outside title+hint zones");

    /* Exact glyph proof: the audio title zone equals a reference render of
     * "Recuperando audio" alone at the same position. */
    memset(ref, 0, (size_t)PANEL_W * PANEL_H * sizeof(uint16_t));
    ui_draw_text_centered(ref, PANEL_W, PANEL_H, 0, 125,
                          MICHI_UI_STR_RECOVERING_AUDIO,
                          michi_ui_font_get(MICHI_FONT_LG),
                          MICHI_UI_TEXT_PRIMARY);
    CHECK(rows_equal(audio, ref, 105, 150),
          "recoverable audio: title is exactly 'Recuperando audio'");
    CHECK(rows_any_pixel(audio, 105, 150),
          "recoverable audio: title zone is non-empty");

    /* Same proof for the network title "Reconectando" (UI-13 wording). */
    memset(ref, 0, (size_t)PANEL_W * PANEL_H * sizeof(uint16_t));
    ui_draw_text_centered(ref, PANEL_W, PANEL_H, 0, 125,
                          MICHI_UI_STR_RECOVERING_TITLE,
                          michi_ui_font_get(MICHI_FONT_LG),
                          MICHI_UI_TEXT_PRIMARY);
    CHECK(rows_equal(net, ref, 105, 150),
          "recoverable network: title is exactly 'Reconectando'");

    free(audio);
    free(net);
    free(ref);
}

/* --------------------------------------------------------------------------
 * Pairing no-PIN wording (Section 17): the pre-PIN pairing screen headline
 * must read "Vinculando", not the legacy "Vincular".
 * -------------------------------------------------------------------------- */

static void test_pairing_linking_wording(void)
{
    printf("michi_ui: pairing no-PIN wording is 'Vinculando'\n");

    const michi_ui_screen_ctx_t pairing_ctx = {
        .state = MICHI_STATE_PAIRING,
        .pairing_pin = NULL,
    };

    uint16_t *pair = malloc((size_t)PANEL_W * PANEL_H * sizeof(uint16_t));
    uint16_t *ref = malloc((size_t)PANEL_W * PANEL_H * sizeof(uint16_t));
    CHECK(pair != NULL && ref != NULL, "pairing wording: fb allocation");
    if (pair == NULL || ref == NULL) {
        free(pair);
        free(ref);
        return;
    }

    memset(pair, 0, (size_t)PANEL_W * PANEL_H * sizeof(uint16_t));
    michi_ui_render_screen(pair, PANEL_W, PANEL_H, 0, &pairing_ctx);

    /* Title zone rows 105..145: exactly "Vinculando". */
    memset(ref, 0, (size_t)PANEL_W * PANEL_H * sizeof(uint16_t));
    ui_draw_text_centered(ref, PANEL_W, PANEL_H, 0, 125,
                          MICHI_UI_STR_PAIRING_LINKING,
                          michi_ui_font_get(MICHI_FONT_LG),
                          MICHI_UI_TEXT_PRIMARY);
    CHECK(rows_equal(pair, ref, 105, 145),
          "pairing no-PIN: title is exactly 'Vinculando'");
    CHECK(rows_any_pixel(pair, 105, 145),
          "pairing no-PIN: title zone is non-empty");

    /* And NOT the legacy "Vincular" (Section 17 wording). */
    memset(ref, 0, (size_t)PANEL_W * PANEL_H * sizeof(uint16_t));
    ui_draw_text_centered(ref, PANEL_W, PANEL_H, 0, 125,
                          MICHI_UI_STR_PAIRING_TITLE,
                          michi_ui_font_get(MICHI_FONT_LG),
                          MICHI_UI_TEXT_PRIMARY);
    CHECK(!rows_equal(pair, ref, 105, 145),
          "pairing no-PIN: title is no longer the legacy 'Vincular'");

    free(pair);
    free(ref);
}

/* --------------------------------------------------------------------------
 * Specific PIN specification test (Section 51)
 * -------------------------------------------------------------------------- */

static void test_pin_specification(void)
{
    printf("michi_ui: PIN layout & typography specification\n");
    const michi_ui_font_t *pin_font = michi_ui_font_get(MICHI_FONT_PIN);
    const michi_ui_font_t *sm_font = michi_ui_font_get(MICHI_FONT_SM);

    /* PIN logical value: 123456 -> visual: 123 456 */
    const char *visual_pin = "123 456";
    int w = ui_text_measure(pin_font, visual_pin);
    int sm_w = ui_text_measure(sm_font, visual_pin);

    CHECK(w <= 210, "PIN visual width fits within 210 px bounding box");
    CHECK(w > 100, "PIN visual width is substantial (> 100 px)");
    CHECK(pin_font->height >= 28, "PIN font height >= 28 px");
    CHECK(w > sm_w * 2, "PIN font is significantly larger than SM font");

    /* Render PIN and verify vertical center around y ≈ 145 (rows 127..163) */
    uint16_t *fb = malloc((size_t)PANEL_W * PANEL_H * sizeof(uint16_t));
    if (fb != NULL) {
        memset(fb, 0, (size_t)PANEL_W * PANEL_H * sizeof(uint16_t));
        michi_ui_draw_pin(fb, PANEL_W, PANEL_H, 0, 145, "123456", MICHI_UI_ACCENT);

        int top_row = -1;
        int bot_row = -1;
        for (int y = 0; y < PANEL_H; y++) {
            for (int x = 0; x < PANEL_W; x++) {
                if (fb[y * PANEL_W + x] != 0) {
                    if (top_row == -1) top_row = y;
                    bot_row = y;
                }
            }
        }
        printf("    PIN bounds: top_row=%d, bot_row=%d, height=%d\n",
               top_row, bot_row, bot_row - top_row + 1);
        CHECK(top_row >= 120 && top_row <= 140, "PIN top row near y=127..137");
        CHECK(bot_row >= 155 && bot_row <= 170, "PIN bottom row near y=163");
        free(fb);
    }
}

/* --------------------------------------------------------------------------
 * Title & artist wrapping tests (Section 52 & 53)
 * -------------------------------------------------------------------------- */

static void test_title_and_artist_wrapping(void)
{
    printf("michi_ui: title & artist wrapping limits\n");
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);
    const michi_ui_font_t *font_md = michi_ui_font_get(MICHI_FONT_MD);

    char buf[128];
    const char *lines[8];

    /* Short title: "Time" -> 1 line */
    strcpy(buf, "Time");
    int n = ui_wrap_text(font_lg, buf, 208, lines, 3);
    CHECK(n == 1, "Short title wraps to 1 line");

    /* Medium title: "Shine On You Crazy Diamond" -> 2 lines */
    strcpy(buf, "Shine On You Crazy Diamond");
    n = ui_wrap_text(font_lg, buf, 208, lines, 3);
    CHECK(n == 2, "Medium title wraps to 2 lines");

    /* Long title: "Everybody Wants to Rule the World (Extended Version Remastered)" -> max 3 lines */
    strcpy(buf, "Everybody Wants to Rule the World (Extended Version Remastered)");
    n = ui_wrap_text(font_lg, buf, 208, lines, 3);
    CHECK(n <= 3, "Long title does not exceed 3 lines");

    /* Artists: max 2 lines */
    strcpy(buf, "Pink Floyd");
    n = ui_wrap_text(font_md, buf, 208, lines, 2);
    CHECK(n == 1, "Short artist wraps to 1 line");

    strcpy(buf, "Creedence Clearwater Revival");
    n = ui_wrap_text(font_md, buf, 208, lines, 2);
    CHECK(n <= 2, "Medium artist wraps to <= 2 lines");

    strcpy(buf, "Orquesta Sinfonica Nacional de Chile");
    n = ui_wrap_text(font_md, buf, 208, lines, 2);
    CHECK(n <= 2, "Long artist wraps to <= 2 lines");
}

/* --------------------------------------------------------------------------
 * Prohibited legacy string checks (Section 50 & 56)
 * -------------------------------------------------------------------------- */

static void test_no_prohibited_strings(void)
{
    printf("michi_ui: verify absence of prohibited legacy strings\n");

    /* Ensure strings header does NOT define prohibited labels */
    CHECK(strcmp(MICHI_UI_STR_READY, "IDLE") != 0, "Ready is not IDLE");
    CHECK(strcmp(MICHI_UI_STR_READY, "Listo") == 0, "Ready is 'Listo'");
    CHECK(strcmp(MICHI_UI_STR_BRAND, "Michi") == 0, "Brand is 'Michi'");
}

int main(void)
{
    test_wrap();
    test_ellipsize();
    test_band_identity();
    test_smoke();
    test_pin_ellipsis();
    test_all_screen_scenarios();
    test_recoverable_error_contextual();
    test_pairing_linking_wording();
    test_pin_specification();
    test_title_and_artist_wrapping();
    test_no_prohibited_strings();

    if (failures == 0) {
        printf("PASS test_michi_ui (all scenarios & contracts green)\n");
        return 0;
    }
    printf("FAIL test_michi_ui (%d failures)\n", failures);
    return 1;
}

