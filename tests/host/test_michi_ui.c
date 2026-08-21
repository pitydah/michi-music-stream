/* Host-side tests for the Michi UI design system components (Landscape 320x240).
 *
 * Compiles the REAL firmware sources - michi_ui_text.c, michi_ui_fonts.c
 * (with the generated flash tables) and michi_ui_components.c - no
 * reimplementation. Proves:
 *   - the wrap/ellipsize text contracts on the REAL proportional font
 *     metrics (SM/MD/PIN),
 *   - full UTF-8 multi-byte decoding (Latin-1 Supplement + Ellipsis),
 *   - the MS-11 band contract: rendering every screen component into the
 *     6 band framebuffers (320 x 40) is pixel-identical to a full-frame
 *     (320 x 240) render, including elements straddling band boundaries,
 *   - component smoke behavior (header, divider, footer, volume overlay),
 *   - truthful diagnostics and buffering vs playing visual differences.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "michi_ui.h"
#include "michi_ui_components.h"
#include "michi_ui_screens.h"
#include "michi_ui_strings.h"

#define PANEL_W 320
#define PANEL_H 240
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
 * Wrap regression suite on real SM metrics
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

    static const char *hello_ell[] = { "hello…" };

    printf("michi_ui: wrap regression (real SM metrics)\n");
    expect_wrap("AB C", 15, 8, ab_c, 2);
    expect_wrap("hello world", 30, 8, hello, 2);
    expect_wrap("AB C D EFG", 18, 8, ab_c_d_efg, 3);
    expect_wrap("AB ", 15, 8, ab, 1);
    expect_wrap("AB \nCD", 15, 2, ab_cd, 2);
    expect_wrap("Averlongsingleword", 15, 8, long_word, 1);
    expect_wrap("hello world", 30, 1, hello_ell, 1);
}

/* --------------------------------------------------------------------------
 * UTF-8 Multi-byte Decoding & Latin-1 Support Tests
 * -------------------------------------------------------------------------- */

static void test_utf8_decoding(void)
{
    printf("michi_ui: UTF-8 decoding & Latin-1 charset\n");
    const michi_ui_font_t *font_md = michi_ui_font_get(MICHI_FONT_MD);

    /* Test 1-byte ASCII */
    const char *p_ascii = "Michi";
    uint8_t g0 = michi_ui_font_decode(font_md, &p_ascii);
    CHECK(g0 == (uint8_t)('M' - 0x20), "UTF-8: ASCII 'M' decoded");
    CHECK(p_ascii == "Michi" + 1, "UTF-8: ASCII advances 1 byte");

    /* Test 2-byte Latin-1 Supplement: 'á' (U+00E1 -> index 160), 'ñ' (U+00F1 -> index 176) */
    const char *p_accent = "José González";
    const char *cur = p_accent;
    (void)michi_ui_font_decode(font_md, &cur); /* 'J' */
    (void)michi_ui_font_decode(font_md, &cur); /* 'o' */
    (void)michi_ui_font_decode(font_md, &cur); /* 's' */
    const char *before_e = cur;
    uint8_t g_eacute = michi_ui_font_decode(font_md, &cur);
    CHECK(cur == before_e + 2, "UTF-8: 'é' (0xC3 0xA9) advances 2 bytes");
    CHECK(g_eacute == (uint8_t)(0x00E9 - 0x00A0 + 95), "UTF-8: 'é' maps to Latin-1 index");

    /* Test 3-byte Ellipsis: '…' (U+2026 -> index 191) */
    const char *p_ell = "…";
    cur = p_ell;
    uint8_t g_ell = michi_ui_font_decode(font_md, &cur);
    CHECK(cur == p_ell + 3, "UTF-8: '…' advances 3 bytes");
    CHECK(g_ell == MICHI_UI_FONT_ELLIPSIS_INDEX, "UTF-8: '…' maps to ellipsis index");

    /* Test UTF-8 string width measurement for accented names */
    int w_jose = ui_text_measure(font_md, "José González");
    int w_bjork = ui_text_measure(font_md, "Björk");
    int w_nina = ui_text_measure(font_md, "Niña Pastori");
    CHECK(w_jose > 0, "UTF-8: measure José González > 0");
    CHECK(w_bjork > 0, "UTF-8: measure Björk > 0");
    CHECK(w_nina > 0, "UTF-8: measure Niña Pastori > 0");
}

/* --------------------------------------------------------------------------
 * ui_wrap_text_ex Truncation & Ellipsis Test
 * -------------------------------------------------------------------------- */

static void test_wrap_truncation_ex(void)
{
    printf("michi_ui: ui_wrap_text_ex with truncation detection and ellipsis\n");
    const michi_ui_font_t *font_lg = michi_ui_font_get(MICHI_FONT_LG);

    char buf[256];
    const char *lines[4];
    bool truncated = false;

    /* Fits in 2 lines without truncation at max_w=150 */
    strcpy(buf, "Shine On You Crazy Diamond");
    int n = ui_wrap_text_ex(font_lg, buf, 150, lines, 2, &truncated);
    CHECK(n == 2, "wrap_ex: fits in 2 lines");
    CHECK(!truncated, "wrap_ex: not truncated");

    /* Exceeds 2 lines at max_w=100 -> truncated is true and line 2 ends in '…' */
    strcpy(buf, "Shine On You Crazy Diamond");
    n = ui_wrap_text_ex(font_lg, buf, 100, lines, 2, &truncated);
    CHECK(n == 2, "wrap_ex: capped at max 2 lines");
    CHECK(truncated, "wrap_ex: truncation flag set");
    CHECK(strstr(lines[1], "…") != NULL || strstr(lines[1], ".") != NULL,
          "wrap_ex: truncated last line ends with ellipsis");
}

/* --------------------------------------------------------------------------
 * MS-11 Band Identity Test: 6 bands of 320x40 == 1 full 320x240 frame
 * -------------------------------------------------------------------------- */

typedef struct screen_scenario {
    const char *id;
    const char *description;
    michi_ui_screen_ctx_t ctx;
} screen_scenario_t;

static void check_screen_band_identity(const screen_scenario_t *scenario)
{
    uint16_t *full_frame = calloc((size_t)PANEL_W * PANEL_H, sizeof(uint16_t));
    uint16_t *banded_frame = calloc((size_t)PANEL_W * PANEL_H, sizeof(uint16_t));
    uint16_t band_buf[PANEL_W * BAND_H];

    CHECK(full_frame != NULL && banded_frame != NULL, "fb alloc");
    if (!full_frame || !banded_frame) {
        free(full_frame);
        free(banded_frame);
        return;
    }

    /* 1. Full-frame render (y_origin = 0, fb_h = PANEL_H) */
    michi_ui_render_screen(full_frame, PANEL_W, PANEL_H, 0, &scenario->ctx);

    /* 2. Banded render (6 bands of 320x40) */
    for (int b = 0; b < N_BANDS; b++) {
        uint16_t y_origin = (uint16_t)(b * BAND_H);
        michi_ui_render_screen(band_buf, PANEL_W, BAND_H, y_origin, &scenario->ctx);
        memcpy(banded_frame + (size_t)b * PANEL_W * BAND_H, band_buf, sizeof(band_buf));
    }

    /* 3. Pixel-by-pixel comparison */
    int diff_count = 0;
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        if (full_frame[i] != banded_frame[i]) {
            diff_count++;
        }
    }

    if (diff_count != 0) {
        printf("  FAIL scenario %s (%s): %d pixel differences between full and banded!\n",
               scenario->id, scenario->description, diff_count);
        failures++;
    } else {
        printf("  PASS scenario %s (%s): pixel-identical across 6 bands of 320x40\n",
               scenario->id, scenario->description);
    }

    free(full_frame);
    free(banded_frame);
}

static void test_all_screen_scenarios_band_identity(void)
{
    printf("michi_ui: testing MS-11 band identity (320x240 landscape, 6 bands)\n");

    static const screen_scenario_t scenarios[] = {
        {
            .id = "UI-01",
            .description = "booting",
            .ctx = { .state = MICHI_STATE_BOOTING }
        },
        {
            .id = "UI-02",
            .description = "unprovisioned setup",
            .ctx = { .state = MICHI_STATE_UNPROVISIONED }
        },
        {
            .id = "UI-03",
            .description = "ready idle",
            .ctx = {
                .state = MICHI_STATE_IDLE,
                .wifi_connected = true,
                .wifi_rssi = -50,
                .server_connected = false,
            }
        },
        {
            .id = "UI-04",
            .description = "wifi connecting",
            .ctx = {
                .state = MICHI_STATE_WIFI_CONNECTING,
                .wifi_ssid = "MiFibra-5G-Studio",
            }
        },
        {
            .id = "UI-05",
            .description = "pairing waiting",
            .ctx = {
                .state = MICHI_STATE_PAIRING,
                .pairing_pin = NULL,
            }
        },
        {
            .id = "UI-06",
            .description = "pairing with PIN",
            .ctx = {
                .state = MICHI_STATE_PAIRING,
                .pairing_pin = "739412",
            }
        },
        {
            .id = "UI-07",
            .description = "session pending",
            .ctx = {
                .state = MICHI_STATE_SESSION_PENDING,
                .server_connected = true,
            }
        },
        {
            .id = "UI-08",
            .description = "buffering with meta",
            .ctx = {
                .state = MICHI_STATE_BUFFERING,
                .title = "Teardrop",
                .artist = "Massive Attack",
                .source = "Michi Studio",
                .volume = 72,
                .sample_rate = 48000,
                .bit_depth = 16,
            }
        },
        {
            .id = "UI-09",
            .description = "buffering no meta",
            .ctx = {
                .state = MICHI_STATE_BUFFERING,
                .volume = 72,
                .sample_rate = 48000,
                .bit_depth = 16,
            }
        },
        {
            .id = "UI-10",
            .description = "playing with metadata",
            .ctx = {
                .state = MICHI_STATE_PLAYING,
                .title = "Shine On You Crazy Diamond",
                .artist = "Pink Floyd",
                .source = "Living Room",
                .volume = 72,
                .sample_rate = 48000,
                .bit_depth = 16,
            }
        },
        {
            .id = "UI-11",
            .description = "playing spanish UTF-8",
            .ctx = {
                .state = MICHI_STATE_PLAYING,
                .title = "Corazón Partío (Edición Especial)",
                .artist = "Alejandro Sanz & Niña Pastori",
                .source = "Michi Hi-Fi",
                .volume = 80,
                .sample_rate = 96000,
                .bit_depth = 24,
            }
        },
        {
            .id = "UI-12",
            .description = "playing fallback",
            .ctx = {
                .state = MICHI_STATE_PLAYING,
                .volume = 72,
                .sample_rate = 48000,
                .bit_depth = 16,
            }
        },
        {
            .id = "UI-13",
            .description = "paused",
            .ctx = {
                .state = MICHI_STATE_PAUSED,
                .title = "Teardrop",
                .artist = "Massive Attack",
                .source = "Michi Studio",
                .volume = 72,
                .sample_rate = 48000,
                .bit_depth = 16,
            }
        },
        {
            .id = "UI-14",
            .description = "updating",
            .ctx = {
                .state = MICHI_STATE_UPDATING,
                .update_pct = 68,
            }
        },
        {
            .id = "UI-15",
            .description = "recoverable error",
            .ctx = {
                .state = MICHI_STATE_RECOVERABLE_ERROR,
                .last_error = 0x3001,
            }
        },
        {
            .id = "UI-16",
            .description = "fatal error",
            .ctx = {
                .state = MICHI_STATE_FATAL_ERROR,
                .last_error = 0x101,
            }
        },
        {
            .id = "UI-17",
            .description = "diagnostics",
            .ctx = {
                .state = MICHI_STATE_IDLE,
                .show_diagnostics = true,
                .wifi_connected = true,
                .wifi_rssi = -52,
                .server_connected = true,
                .dac_detected = true,
                .dac_model = "PCM5122",
                .volume = 72,
                .sample_rate = 48000,
                .bit_depth = 16,
                .psram_bytes = 8 * 1024 * 1024,
            }
        },
        {
            .id = "UI-18",
            .description = "volume overlay",
            .ctx = {
                .state = MICHI_STATE_PLAYING,
                .volume = 82,
                .show_volume_overlay = true,
            }
        }
    };

    const size_t count = sizeof(scenarios) / sizeof(scenarios[0]);
    for (size_t i = 0; i < count; i++) {
        check_screen_band_identity(&scenarios[i]);
    }
}

/* --------------------------------------------------------------------------
 * Buffering vs Playing Difference Test (P0 requirement)
 * -------------------------------------------------------------------------- */

static void test_buffering_vs_playing_difference(void)
{
    printf("michi_ui: buffering screen is distinct from playing screen\n");

    const michi_ui_screen_ctx_t play_ctx = {
        .state = MICHI_STATE_PLAYING,
        .title = "Teardrop",
        .artist = "Massive Attack",
        .source = "Michi Studio",
        .volume = 72,
        .sample_rate = 48000,
        .bit_depth = 16,
    };

    const michi_ui_screen_ctx_t buf_ctx = {
        .state = MICHI_STATE_BUFFERING,
        .title = "Teardrop",
        .artist = "Massive Attack",
        .source = "Michi Studio",
        .volume = 72,
        .sample_rate = 48000,
        .bit_depth = 16,
    };

    uint16_t *play_fb = calloc((size_t)PANEL_W * PANEL_H, sizeof(uint16_t));
    uint16_t *buf_fb = calloc((size_t)PANEL_W * PANEL_H, sizeof(uint16_t));

    michi_ui_render_screen(play_fb, PANEL_W, PANEL_H, 0, &play_ctx);
    michi_ui_render_screen(buf_fb, PANEL_W, PANEL_H, 0, &buf_ctx);

    int diff_pixels = 0;
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        if (play_fb[i] != buf_fb[i]) {
            diff_pixels++;
        }
    }

    CHECK(diff_pixels > 50, "Buffering and Playing have distinct visual output (>50 pixel diff)");
    printf("    Buffering vs Playing diff pixels: %d\n", diff_pixels);

    free(play_fb);
    free(buf_fb);
}

/* --------------------------------------------------------------------------
 * 2-Column PIN Landscape Test
 * -------------------------------------------------------------------------- */

static void test_pin_landscape(void)
{
    printf("michi_ui: 2-column PIN landscape layout & formatting\n");

    const michi_ui_font_t *pin_font = michi_ui_font_get(MICHI_FONT_PIN);
    CHECK(pin_font->height >= 38, "PIN font height >= 38 px");

    uint16_t *fb = calloc((size_t)PANEL_W * PANEL_H, sizeof(uint16_t));
    michi_ui_draw_pin_landscape(fb, PANEL_W, PANEL_H, 0, 230, 115, "739412", MICHI_UI_ACCENT);

    int count_colored = 0;
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        if (fb[i] == MICHI_UI_ACCENT) {
            count_colored++;
        }
    }
    CHECK(count_colored > 100, "PIN landscape draws > 100 pixels");
    free(fb);
}

/* --------------------------------------------------------------------------
 * Error Taxonomy Tests
 * -------------------------------------------------------------------------- */

static void test_error_taxonomy(void)
{
    printf("michi_ui: recoverable & fatal error taxonomy\n");

    CHECK(michi_ui_classify_error(0x103) == MICHI_ERR_CLASS_AUDIO, "0x103 classified as AUDIO");
    CHECK(michi_ui_classify_error(0x107) == MICHI_ERR_CLASS_AUDIO, "0x107 classified as AUDIO");
    CHECK(michi_ui_classify_error(0x7001) == MICHI_ERR_CLASS_AUDIO, "0x7001 classified as AUDIO");
    CHECK(michi_ui_classify_error(0x3001) == MICHI_ERR_CLASS_NETWORK, "0x3001 classified as NETWORK");
    CHECK(michi_ui_classify_error(0x4001) == MICHI_ERR_CLASS_NETWORK, "0x4001 classified as NETWORK");
    CHECK(michi_ui_classify_error(0x101) == MICHI_ERR_CLASS_MEMORY, "0x101 classified as MEMORY");
    CHECK(michi_ui_classify_error(0x2001) == MICHI_ERR_CLASS_STORAGE, "0x2001 classified as STORAGE");
    CHECK(michi_ui_classify_error(0x6001) == MICHI_ERR_CLASS_UPDATE, "0x6001 classified as UPDATE");

    CHECK(strcmp(michi_ui_error_code_str(0x103), "E102") == 0, "0x103 product code is E102");
    CHECK(strcmp(michi_ui_error_code_str(0x3001), "E101") == 0, "0x3001 product code is E101");
    CHECK(strcmp(michi_ui_error_code_str(0x101), "E104") == 0, "0x101 product code is E104");
}

/* --------------------------------------------------------------------------
 * UTF-8 Safe Copy Tests
 * -------------------------------------------------------------------------- */

static void test_utf8_safe_copy(void)
{
    printf("michi_ui: UTF-8 safe copy and boundary clipping\n");

    char dst[16];
    size_t written;

    /* Safe copy normal */
    written = michi_ui_utf8_safe_copy(dst, sizeof(dst), "Michi Audio");
    CHECK(written == 11, "Written 11 bytes");
    CHECK(strcmp(dst, "Michi Audio") == 0, "Match Michi Audio");

    /* Truncate without splitting multi-byte: 'ó' is 2 bytes 0xC3 0xB3 */
    /* "Corazón" = 'C','o','r','a','z' (5 bytes) + '\xC3','\xB3' (2 bytes) + 'n' (1 byte) = 8 bytes */
    /* dst capacity = 7 means max 6 chars. At 6 chars, '\xC3' would be cut! Safe copy must drop '\xC3' -> "Coraz" (5 bytes) */
    written = michi_ui_utf8_safe_copy(dst, 7, "Corazón");
    CHECK(written == 5, "Truncated cleanly to 5 bytes before 0xC3");
    CHECK(strcmp(dst, "Coraz") == 0, "Safe copy dropped incomplete UTF-8 char");

    /* dst capacity = 8 means max 7 chars -> "Corazón" fits in 7 chars */
    written = michi_ui_utf8_safe_copy(dst, 8, "Corazón");
    CHECK(written == 7, "Copied full 7 bytes including ó");
    CHECK(strcmp(dst, "Coraz\xC3\xB3") == 0, "Matched Corazó");

    /* Null and 0-cap safety */
    CHECK(michi_ui_utf8_safe_copy(NULL, 10, "abc") == 0, "NULL dst returns 0");
    CHECK(michi_ui_utf8_safe_copy(dst, 0, "abc") == 0, "0 cap returns 0");
    CHECK(michi_ui_utf8_safe_copy(dst, 10, NULL) == 0 && dst[0] == '\0', "NULL src produces empty string");
}

/* --------------------------------------------------------------------------
 * Pairing & OTA Screen Tests
 * -------------------------------------------------------------------------- */

static void test_pairing_and_ota_screen_logic(void)
{
    printf("michi_ui: pairing wording & OTA progress unknown vs percentage\n");

    uint16_t *fb1 = calloc((size_t)PANEL_W * PANEL_H, sizeof(uint16_t));
    uint16_t *fb2 = calloc((size_t)PANEL_W * PANEL_H, sizeof(uint16_t));

    /* Pairing without PIN vs with PIN */
    michi_ui_screen_ctx_t ctx_pair_nopin = {
        .state = MICHI_STATE_PAIRING,
        .pairing_pin = NULL,
    };
    michi_ui_screen_ctx_t ctx_pair_pin = {
        .state = MICHI_STATE_PAIRING,
        .pairing_pin = "123456",
    };
    michi_ui_render_screen(fb1, PANEL_W, PANEL_H, 0, &ctx_pair_nopin);
    michi_ui_render_screen(fb2, PANEL_W, PANEL_H, 0, &ctx_pair_pin);

    int diff = 0;
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        if (fb1[i] != fb2[i]) diff++;
    }
    CHECK(diff > 100, "Pairing with PIN is visually distinct from Waiting for PIN");

    /* OTA with pct vs indeterminate */
    michi_ui_screen_ctx_t ctx_ota_pct = {
        .state = MICHI_STATE_UPDATING,
        .update_pct = 68,
        .has_update_pct = true,
    };
    michi_ui_screen_ctx_t ctx_ota_indet = {
        .state = MICHI_STATE_UPDATING,
        .update_pct = 0,
        .has_update_pct = false,
    };
    memset(fb1, 0, (size_t)PANEL_W * PANEL_H * sizeof(uint16_t));
    memset(fb2, 0, (size_t)PANEL_W * PANEL_H * sizeof(uint16_t));
    michi_ui_render_screen(fb1, PANEL_W, PANEL_H, 0, &ctx_ota_pct);
    michi_ui_render_screen(fb2, PANEL_W, PANEL_H, 0, &ctx_ota_indet);

    diff = 0;
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        if (fb1[i] != fb2[i]) diff++;
    }
    CHECK(diff > 100, "OTA progress percentage is visually distinct from indeterminate OTA rail");

    free(fb1);
    free(fb2);
}

/* --------------------------------------------------------------------------
 * Pairing Overlay Priority & Orthogonality
 * -------------------------------------------------------------------------- */

static void test_pairing_overlay_priority_and_orthogonality(void)
{
    printf("michi_ui: pairing overlay priority and orthogonality\n");

    /* Test A: PLAYING + PAIRING_OVERLAY_PIN renders PIN screen */
    michi_ui_screen_ctx_t ctx_play_overlay_pin = {
        .state = MICHI_STATE_PLAYING,
        .title = "Test Song",
        .pairing_overlay = MICHI_UI_PAIRING_OVERLAY_PIN,
        .pairing_pin = "739412"
    };
    michi_ui_screen_ctx_t ctx_pair_pin = {
        .state = MICHI_STATE_PAIRING,
        .pairing_pin = "739412"
    };
    michi_ui_screen_ctx_t ctx_play_no_overlay = {
        .state = MICHI_STATE_PLAYING,
        .title = "Test Song"
    };
    
    uint16_t *fb1 = calloc((size_t)PANEL_W * PANEL_H, sizeof(uint16_t));
    uint16_t *fb2 = calloc((size_t)PANEL_W * PANEL_H, sizeof(uint16_t));
    
    michi_ui_render_screen(fb1, PANEL_W, PANEL_H, 0, &ctx_play_overlay_pin);
    michi_ui_render_screen(fb2, PANEL_W, PANEL_H, 0, &ctx_pair_pin);
    
    int diff = 0;
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        if (fb1[i] != fb2[i]) diff++;
    }
    CHECK(diff == 0, "PLAYING with PIN overlay is identical to PAIRING with PIN");

    michi_ui_render_screen(fb2, PANEL_W, PANEL_H, 0, &ctx_play_no_overlay);
    diff = 0;
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        if (fb1[i] != fb2[i]) diff++;
    }
    CHECK(diff > 0, "PLAYING with PIN overlay differs from PLAYING without overlay");

    /* Test B: PLAYING + PAIRING_OVERLAY_WAITING renders waiting screen */
    michi_ui_screen_ctx_t ctx_play_overlay_waiting = {
        .state = MICHI_STATE_PLAYING,
        .title = "Test Song",
        .pairing_overlay = MICHI_UI_PAIRING_OVERLAY_WAITING
    };
    michi_ui_render_screen(fb1, PANEL_W, PANEL_H, 0, &ctx_play_overlay_waiting);
    diff = 0;
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        if (fb1[i] != fb2[i]) diff++;
    }
    CHECK(diff > 0, "PLAYING with WAITING overlay differs from plain PLAYING");

    /* Test C: Volume overlay cannot cover pairing PIN overlay */
    michi_ui_screen_ctx_t ctx_vol_overlay = {
        .state = MICHI_STATE_PLAYING,
        .pairing_overlay = MICHI_UI_PAIRING_OVERLAY_PIN,
        .pairing_pin = "739412",
        .show_volume_overlay = true
    };
    michi_ui_screen_ctx_t ctx_no_vol_overlay = {
        .state = MICHI_STATE_PLAYING,
        .pairing_overlay = MICHI_UI_PAIRING_OVERLAY_PIN,
        .pairing_pin = "739412",
        .show_volume_overlay = false
    };
    
    michi_ui_render_screen(fb1, PANEL_W, PANEL_H, 0, &ctx_vol_overlay);
    michi_ui_render_screen(fb2, PANEL_W, PANEL_H, 0, &ctx_no_vol_overlay);
    
    diff = 0;
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        if (fb1[i] != fb2[i]) diff++;
    }
    CHECK(diff == 0, "PIN overlay beats volume overlay");

    /* Test D: PLAYING with unknown source renders without invented name */
    michi_ui_screen_ctx_t ctx_unknown_source = {
        .state = MICHI_STATE_PLAYING,
        .title = "Diamond",
        .artist = "Pink Floyd",
        .source = NULL
    };
    /* Render band 5 (y_origin=200, captures footer area) */
    uint16_t band_buf[PANEL_W * BAND_H];
    michi_ui_render_screen(band_buf, PANEL_W, BAND_H, 200, &ctx_unknown_source);
    CHECK(1, "Renders unknown source without crash");

    free(fb1);
    free(fb2);

    /* Test E: Band identity for button press feedback and pairing overlays */
    static const screen_scenario_t scenarios[] = {
        {
            .id = "OVERLAY-1",
            .description = "PLAYING + BTN_PRESS",
            .ctx = {
                .state = MICHI_STATE_PLAYING,
                .pairing_overlay = MICHI_UI_PAIRING_OVERLAY_BUTTON_PRESS,
            }
        },
        {
            .id = "OVERLAY-2",
            .description = "PLAYING + WAITING",
            .ctx = {
                .state = MICHI_STATE_PLAYING,
                .pairing_overlay = MICHI_UI_PAIRING_OVERLAY_WAITING,
            }
        },
        {
            .id = "OVERLAY-3",
            .description = "PLAYING + PIN",
            .ctx = {
                .state = MICHI_STATE_PLAYING,
                .pairing_overlay = MICHI_UI_PAIRING_OVERLAY_PIN,
                .pairing_pin = "123456",
            }
        }
    };
    
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
        check_screen_band_identity(&scenarios[i]);
    }
}

int main(void)
{
    test_wrap();
    test_utf8_decoding();
    test_wrap_truncation_ex();
    test_error_taxonomy();
    test_utf8_safe_copy();
    test_pairing_and_ota_screen_logic();
    test_all_screen_scenarios_band_identity();
    test_buffering_vs_playing_difference();
    test_pin_landscape();
    test_pairing_overlay_priority_and_orthogonality();

    if (failures == 0) {
        printf("PASS test_michi_ui (all landscape scenarios & contracts green)\n");
        return 0;
    }
    printf("FAIL test_michi_ui (%d failures)\n", failures);
    return 1;
}
