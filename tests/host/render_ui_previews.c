#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "michi_ui.h"
#include "michi_ui_screens.h"

#define PANEL_W 320
#define PANEL_H 240
#define BAND_H 40
#define N_BANDS (PANEL_H / BAND_H)

static void save_ppm(const char *path, const uint16_t *fb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Failed to open %s for writing\n", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint16_t rgb565 = fb[i];
        uint8_t r = ((rgb565 >> 11) & 0x1F) * 255 / 31;
        uint8_t g = ((rgb565 >> 5) & 0x3F) * 255 / 63;
        uint8_t b = (rgb565 & 0x1F) * 255 / 31;
        fputc(r, f);
        fputc(g, f);
        fputc(b, f);
    }
    fclose(f);
    printf("Saved PPM preview: %s\n", path);
}

static void render_scenario_to_file(const char *filename, const michi_ui_screen_ctx_t *ctx, const char *out_dir)
{
    char fullpath[512];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", out_dir, filename);

    uint16_t *full_frame = calloc((size_t)PANEL_W * PANEL_H, sizeof(uint16_t));
    if (!full_frame) return;

    /* Render through banded pipeline (6 bands of 320x40) to verify exact hardware execution */
    uint16_t band_buf[PANEL_W * BAND_H];
    for (int b = 0; b < N_BANDS; b++) {
        uint16_t y_origin = (uint16_t)(b * BAND_H);
        michi_ui_render_screen(band_buf, PANEL_W, BAND_H, y_origin, ctx);
        memcpy(full_frame + (size_t)b * PANEL_W * BAND_H, band_buf, sizeof(band_buf));
    }

    save_ppm(fullpath, full_frame, PANEL_W, PANEL_H);
    free(full_frame);
}

int main(int argc, char **argv)
{
    const char *out_dir = "previews";
    if (argc > 1) {
        out_dir = argv[1];
    }
    mkdir(out_dir, 0755);

    /* 1. Boot */
    michi_ui_screen_ctx_t s01 = { .state = MICHI_STATE_BOOTING };
    render_scenario_to_file("ui-01-boot.ppm", &s01, out_dir);

    /* 2. Unprovisioned */
    michi_ui_screen_ctx_t s02 = { .state = MICHI_STATE_UNPROVISIONED };
    render_scenario_to_file("ui-02-unprovisioned.ppm", &s02, out_dir);

    /* 3. Ready */
    michi_ui_screen_ctx_t s03 = {
        .state = MICHI_STATE_IDLE,
        .wifi_connected = true,
        .wifi_rssi = -48,
        .server_connected = false,
    };
    render_scenario_to_file("ui-03-ready.ppm", &s03, out_dir);

    /* 4. Connecting */
    michi_ui_screen_ctx_t s04 = {
        .state = MICHI_STATE_WIFI_CONNECTING,
        .wifi_ssid = "Michi-Studio-5G",
    };
    render_scenario_to_file("ui-04-connecting.ppm", &s04, out_dir);

    /* 5. Pairing waiting */
    michi_ui_screen_ctx_t s05 = {
        .state = MICHI_STATE_PAIRING,
        .pairing_pin = NULL,
    };
    render_scenario_to_file("ui-05-pairing-waiting.ppm", &s05, out_dir);

    /* 6. Pairing with PIN */
    michi_ui_screen_ctx_t s06 = {
        .state = MICHI_STATE_PAIRING,
        .pairing_pin = "739412",
    };
    render_scenario_to_file("ui-06-pairing-pin.ppm", &s06, out_dir);

    /* 7. Session pending */
    michi_ui_screen_ctx_t s07 = {
        .state = MICHI_STATE_SESSION_PENDING,
        .server_connected = true,
    };
    render_scenario_to_file("ui-07-session-pending.ppm", &s07, out_dir);

    /* 8. Buffering with metadata */
    michi_ui_screen_ctx_t s08 = {
        .state = MICHI_STATE_BUFFERING,
        .title = "Teardrop",
        .artist = "Massive Attack",
        .source = "Michi Studio",
        .volume = 72,
        .sample_rate = 48000,
        .bit_depth = 16,
    };
    render_scenario_to_file("ui-08-buffering-meta.ppm", &s08, out_dir);

    /* 9. Buffering without metadata */
    michi_ui_screen_ctx_t s09 = {
        .state = MICHI_STATE_BUFFERING,
        .volume = 72,
        .sample_rate = 48000,
        .bit_depth = 16,
    };
    render_scenario_to_file("ui-09-buffering-nometa.ppm", &s09, out_dir);

    /* 10. Playing standard track */
    michi_ui_screen_ctx_t s10 = {
        .state = MICHI_STATE_PLAYING,
        .title = "Shine On You Crazy Diamond (Pts. 1-5)",
        .artist = "Pink Floyd",
        .source = "Living Room",
        .volume = 72,
        .sample_rate = 48000,
        .bit_depth = 16,
    };
    render_scenario_to_file("ui-10-playing-meta.ppm", &s10, out_dir);

    /* 11. Playing with Spanish UTF-8 accents */
    michi_ui_screen_ctx_t s11 = {
        .state = MICHI_STATE_PLAYING,
        .title = "Corazón Partío (Edición Especial)",
        .artist = "Alejandro Sanz & Niña Pastori",
        .source = "Michi Hi-Fi",
        .volume = 80,
        .sample_rate = 96000,
        .bit_depth = 24,
    };
    render_scenario_to_file("ui-11-playing-spanish-utf8.ppm", &s11, out_dir);

    /* 12. Playing fallback (no metadata) */
    michi_ui_screen_ctx_t s12 = {
        .state = MICHI_STATE_PLAYING,
        .volume = 72,
        .sample_rate = 48000,
        .bit_depth = 16,
    };
    render_scenario_to_file("ui-12-playing-fallback.ppm", &s12, out_dir);

    /* 13. Paused with metadata */
    michi_ui_screen_ctx_t s13 = {
        .state = MICHI_STATE_PAUSED,
        .title = "Teardrop",
        .artist = "Massive Attack",
        .source = "Michi Studio",
        .volume = 72,
        .sample_rate = 48000,
        .bit_depth = 16,
    };
    render_scenario_to_file("ui-13-paused-meta.ppm", &s13, out_dir);

    /* 14. Paused fallback */
    michi_ui_screen_ctx_t s14 = {
        .state = MICHI_STATE_PAUSED,
        .volume = 72,
        .sample_rate = 48000,
        .bit_depth = 16,
    };
    render_scenario_to_file("ui-14-paused-nometa.ppm", &s14, out_dir);

    /* 15. Updating progress */
    michi_ui_screen_ctx_t s15 = {
        .state = MICHI_STATE_UPDATING,
        .update_pct = 68,
    };
    render_scenario_to_file("ui-15-updating-progress.ppm", &s15, out_dir);

    /* 16. Recoverable error (Audio) */
    michi_ui_screen_ctx_t s16 = {
        .state = MICHI_STATE_RECOVERABLE_ERROR,
        .last_error = 0x3002,
    };
    render_scenario_to_file("ui-16-recoverable-error-audio.ppm", &s16, out_dir);

    /* 17. Recoverable error (Network) */
    michi_ui_screen_ctx_t s17 = {
        .state = MICHI_STATE_RECOVERABLE_ERROR,
        .last_error = 0x1002,
    };
    render_scenario_to_file("ui-17-recoverable-error-network.ppm", &s17, out_dir);

    /* 18. Fatal error */
    michi_ui_screen_ctx_t s18 = {
        .state = MICHI_STATE_FATAL_ERROR,
        .last_error = 0xE104,
    };
    render_scenario_to_file("ui-18-fatal-error.ppm", &s18, out_dir);

    /* 19. Diagnostics connected */
    michi_ui_screen_ctx_t s19 = {
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
        .fw_version = "v0.2.0",
    };
    render_scenario_to_file("ui-19-diagnostics-connected.ppm", &s19, out_dir);

    /* 20. Volume overlay */
    michi_ui_screen_ctx_t s20 = {
        .state = MICHI_STATE_PLAYING,
        .volume = 82,
        .show_volume_overlay = true,
    };
    render_scenario_to_file("ui-20-volume-overlay.ppm", &s20, out_dir);

    printf("Generated all 20 UI previews in %s/\n", out_dir);
    return 0;
}
