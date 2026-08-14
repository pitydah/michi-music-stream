#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "michi_board.h"
#include "michi_display.h"
#include "michi_product_profile.h"
#include "michi_state.h"
#include "michi_version.h"
#include "michi_volume.h"

#define TAG "michi_display"

/* Render task priority: below the FSM task (5) so the event bus is never
 * starved, and below the I2S consumer (8, phase 11) so a full-frame flush
 * (~65-80 ms) never delays audio; above app_main (1). */
#define MICHI_DISPLAY_TASK_PRIORITY 4

/* Font metrics: embedded 5x7 font with 6 px pitch (MICHI_TEXT_SPACING in
 * the BSP); the framebuffer is the BSP's 240x320 RGB565. */
#define MICHI_CHAR_PITCH 6
#define MICHI_CHAR_H 7
#define MICHI_HEADER_Y 8
#define MICHI_FOOTER_Y (320 - MICHI_CHAR_H - 9)
#define MICHI_BODY_Y0 28
#define MICHI_BODY_PITCH 26

#define MICHI_COLOR_WHITE 0xFFFF
#define MICHI_COLOR_DIM 0x8410

/* One body line: 38 chars * 6 px + 6 px margin = 234 px, fits 240. */
#define MICHI_LINE_CHARS 38
/* First title line: "Title: " label (7 chars) + up to 31 title chars
 * (29 visible + ".." marker). */
#define MICHI_TITLE_LINE1_CHARS (MICHI_LINE_CHARS - 7)
/* Visible chars on the first title line (the marker consumes 2). */
#define MICHI_TITLE_LINE1_VISIBLE (MICHI_TITLE_LINE1_CHARS - 2)
/* Second title line (no label): up to 33 chars (31 visible + ".." marker);
 * keeps the label+value+NUL <= 41 invariant with the 34-byte value buffer. */
#define MICHI_TITLE_LINE2_CHARS (MICHI_TITLE_LINE1_CHARS + 2)

/* Initial applied format (48000/16/2); the session layer (phase 11/12) will
 * drive the real value once negotiated. */
#define MICHI_DISPLAY_APPLIED_BIT_DEPTH 16

/* Queue payload: request type (0 = full re-render). */
#define MICHI_DISPLAY_MSG_RENDER 0

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static volatile bool s_initialized;

/* Set when a render request is dropped on a full queue; the render task
 * re-renders once after the drain so a dropped request does not leave the
 * screen stale. */
static volatile bool s_pending;

/* Now-playing info + last error: written by producers (the observer on the
 * FSM task, the session layer in phase 12) and read by the render task. A
 * portMUX critical section keeps every access non-blocking (observer
 * contract: MUST NOT block). */
static portMUX_TYPE s_info_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_source[MICHI_DISPLAY_SOURCE_MAX + 1];
static char s_title[MICHI_DISPLAY_TITLE_MAX + 1];
static char s_artist[MICHI_DISPLAY_ARTIST_MAX + 1];
static uint32_t s_last_error;
/* Pairing PIN (MS-06): 6 digits, shown ONLY on the local panel, never
 * returned by HTTP. Empty when no active PIN. */
static char s_pairing_pin[7];

static void queue_render(void)
{
    const uint32_t msg = MICHI_DISPLAY_MSG_RENDER;
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        s_pending = true;
        ESP_LOGW(TAG, "display: queue_full dropped=1");
    }
}

/* Observer contract (invoked from the FSM task): queue ONLY, never render,
 * never block. The render task filters and draws. */
static void on_state_event(const michi_event_t *ev)
{
    if (ev->id == MICHI_EVENT_STATE_CHANGED) {
        queue_render();
        return;
    }
    if (ev->id == MICHI_EVENT_ERROR) {
        portENTER_CRITICAL(&s_info_mux);
        s_last_error = ev->data;
        portEXIT_CRITICAL(&s_info_mux);
        queue_render();
    }
}

static void draw_centered(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, int y,
                          const char *str, uint16_t color)
{
    int x = ((int)fb_w - (int)strlen(str) * MICHI_CHAR_PITCH) / 2;
    if (x < 0) {
        x = 0;
    }
    michi_board_display_draw_text(fb, fb_w, fb_h, x, y, str, color, 0x0000);
}

/* Copy at most max_chars chars from src, appending ".." when truncated.
 * Always NUL-terminates. */
static void copy_limited(char *dst, size_t dst_cap, const char *src, size_t max_chars)
{
    size_t n = strlen(src);
    bool truncated = n > max_chars;
    if (truncated) {
        n = max_chars;
        if (n >= 2) {
            n -= 2; /* room for the ".." marker */
        }
    }
    if (n >= dst_cap) {
        n = dst_cap - 1;
    }
    memcpy(dst, src, n);
    size_t idx = n;
    if (truncated && idx + 2 < dst_cap) {
        dst[idx++] = '.';
        dst[idx++] = '.';
    }
    dst[idx] = '\0';
}

/* Bounded NUL-terminated copy for the now-playing buffers: strnlen + memcpy
 * keeps the critical section at fixed cost (no formatting under the lock).
 * NULL/empty input renders as "--" downstream. */
static void copy_bounded(char *dst, size_t dst_cap, const char *src)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strnlen(src, dst_cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Source/Title/Artist/format/Wi-Fi/Vol block shared by PLAYING (white) and
 * PAUSED (dimmed).
 *
 * Buffer sizes are chosen so snprintf("Label: %s") can never truncate:
 * "Source: "/"Artist: " (8 chars) need a value buffer <= 33 bytes, "Title: "
 * (7 chars) one <= 34 (dest is 41 bytes). */
static void render_playing_lines(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                 int y, uint16_t color)
{
    const michi_product_profile_t *p = michi_product_profile_get();
    char line[MICHI_LINE_CHARS + 3];
    char buf[34];
    char short_buf[33];

    /* Snapshot the CONTENT under the lock (the writer updates these buffers
     * under the same lock); all formatting/drawing happens outside with the
     * local copies - pointers are never dereferenced outside the lock. */
    char src[MICHI_DISPLAY_SOURCE_MAX + 1];
    char title[MICHI_DISPLAY_TITLE_MAX + 1];
    char artist[MICHI_DISPLAY_ARTIST_MAX + 1];
    portENTER_CRITICAL(&s_info_mux);
    copy_bounded(src, sizeof(src), s_source);
    copy_bounded(title, sizeof(title), s_title);
    copy_bounded(artist, sizeof(artist), s_artist);
    portEXIT_CRITICAL(&s_info_mux);

    /* Source: 8-char label + up to 30 chars. */
    copy_limited(short_buf, sizeof(short_buf), src, MICHI_LINE_CHARS - 8);
    snprintf(line, sizeof(line), "Source: %s",
             short_buf[0] != '\0' ? short_buf : "--");
    draw_centered(fb, fb_w, fb_h, y, line, color);
    y += MICHI_BODY_PITCH;

    /* Title: 7-char label + up to 31 chars; wraps to a second line with the
     * remainder (max 2 lines). The second line continues at the visible
     * count of line 1 (the ".." marker is not part of the text). */
    if (title[0] == '\0') {
        draw_centered(fb, fb_w, fb_h, y, "Title: --", color);
        y += MICHI_BODY_PITCH;
    } else {
        copy_limited(buf, sizeof(buf), title, MICHI_TITLE_LINE1_CHARS);
        snprintf(line, sizeof(line), "Title: %s", buf);
        draw_centered(fb, fb_w, fb_h, y, line, color);
        y += MICHI_BODY_PITCH;
        if (strlen(title) > MICHI_TITLE_LINE1_VISIBLE) {
            copy_limited(buf, sizeof(buf), title + MICHI_TITLE_LINE1_VISIBLE,
                         MICHI_TITLE_LINE2_CHARS);
            draw_centered(fb, fb_w, fb_h, y, buf, color);
            y += MICHI_BODY_PITCH;
        }
    }

    copy_limited(short_buf, sizeof(short_buf), artist, MICHI_LINE_CHARS - 8);
    snprintf(line, sizeof(line), "Artist: %s",
             short_buf[0] != '\0' ? short_buf : "--");
    draw_centered(fb, fb_w, fb_h, y, line, color);
    y += MICHI_BODY_PITCH;

    /* Format: the REAL validated sample rate from the product profile and
     * the initial applied bit depth (48000/16/2); the session layer
     * (phase 11/12) will drive the real value once negotiated. */
    snprintf(line, sizeof(line), "%" PRIu32 " kHz / %u-bit",
             p->validated_sample_rate / 1000u,
             (unsigned)MICHI_DISPLAY_APPLIED_BIT_DEPTH);
    draw_centered(fb, fb_w, fb_h, y, line, color);
    y += MICHI_BODY_PITCH;

    /* Wi-Fi placeholder: the network phase (9) fills the value; this
     * subsystem only renders the state. */
    draw_centered(fb, fb_w, fb_h, y, "Wi-Fi: --", color);
    y += MICHI_BODY_PITCH;

    snprintf(line, sizeof(line), "Vol: %u", (unsigned)michi_volume_get());
    draw_centered(fb, fb_w, fb_h, y, line, color);
    y += MICHI_BODY_PITCH;
}

/* Draw callback for michi_board_display_render(): header + footer + the
 * screen of the CURRENT state. Runs on the render task only. */
static void render_frame(uint16_t *fb, uint16_t fb_w, uint16_t fb_h)
{
    const michi_product_profile_t *p = michi_product_profile_get();

    draw_centered(fb, fb_w, fb_h, MICHI_HEADER_Y, p->product_name,
                  MICHI_COLOR_WHITE);
    draw_centered(fb, fb_w, fb_h, MICHI_FOOTER_Y, "v" MICHI_FW_VERSION_STR,
                  MICHI_COLOR_DIM);

    switch (michi_state_get()) {
    case MICHI_STATE_IDLE:
        draw_centered(fb, fb_w, fb_h, 140, "IDLE", MICHI_COLOR_WHITE);
        draw_centered(fb, fb_w, fb_h, 156, "Ready to pair", MICHI_COLOR_WHITE);
        break;
    case MICHI_STATE_UNPROVISIONED:
        draw_centered(fb, fb_w, fb_h, 140, "Not configured", MICHI_COLOR_WHITE);
        draw_centered(fb, fb_w, fb_h, 156, "Press pairing button", MICHI_COLOR_WHITE);
        break;
    case MICHI_STATE_PROVISIONING:
    case MICHI_STATE_WIFI_CONNECTING:
        draw_centered(fb, fb_w, fb_h, 148, "Connecting...", MICHI_COLOR_WHITE);
        break;
    case MICHI_STATE_PAIRING: {
        /* Snapshot the PIN under the lock (same contract as the
         * now-playing buffers); render the PIN when set, else the
         * waiting hint. */
        char pin[7];
        portENTER_CRITICAL(&s_info_mux);
        copy_bounded(pin, sizeof(pin), s_pairing_pin);
        portEXIT_CRITICAL(&s_info_mux);
        if (pin[0] != '\0') {
            char line[MICHI_LINE_CHARS + 3];
            snprintf(line, sizeof(line), "Pairing PIN: %s", pin);
            draw_centered(fb, fb_w, fb_h, 140, line, MICHI_COLOR_WHITE);
        } else {
            draw_centered(fb, fb_w, fb_h, 140, "Pairing...", MICHI_COLOR_WHITE);
            draw_centered(fb, fb_w, fb_h, 156, "Waiting for confirmation",
                          MICHI_COLOR_WHITE);
        }
        break;
    }
    case MICHI_STATE_SESSION_PENDING:
    case MICHI_STATE_BUFFERING:
        draw_centered(fb, fb_w, fb_h, 148, "Buffering...", MICHI_COLOR_WHITE);
        break;
    case MICHI_STATE_PLAYING:
        render_playing_lines(fb, fb_w, fb_h, MICHI_BODY_Y0, MICHI_COLOR_WHITE);
        break;
    case MICHI_STATE_PAUSED:
        draw_centered(fb, fb_w, fb_h, MICHI_BODY_Y0, "Paused", MICHI_COLOR_WHITE);
        render_playing_lines(fb, fb_w, fb_h, MICHI_BODY_Y0 + MICHI_BODY_PITCH,
                             MICHI_COLOR_DIM);
        break;
    case MICHI_STATE_UPDATING:
        draw_centered(fb, fb_w, fb_h, 148, "Updating firmware...", MICHI_COLOR_WHITE);
        break;
    case MICHI_STATE_RECOVERABLE_ERROR:
        draw_centered(fb, fb_w, fb_h, 140, "Recovering...", MICHI_COLOR_WHITE);
        draw_centered(fb, fb_w, fb_h, 156, "Auto retry in progress", MICHI_COLOR_WHITE);
        break;
    case MICHI_STATE_FATAL_ERROR:
        draw_centered(fb, fb_w, fb_h, 132, "FATAL ERROR", MICHI_COLOR_WHITE);
        portENTER_CRITICAL(&s_info_mux);
        const uint32_t last_err = s_last_error;
        portEXIT_CRITICAL(&s_info_mux);
        if (last_err != 0) {
            draw_centered(fb, fb_w, fb_h, 150,
                          esp_err_to_name((esp_err_t)last_err), MICHI_COLOR_DIM);
        } else {
            draw_centered(fb, fb_w, fb_h, 150, "See serial log", MICHI_COLOR_DIM);
        }
        break;
    case MICHI_STATE_BOOTING:
    case MICHI_STATE_SELF_TEST:
    default:
        break; /* covered by the BSP boot screen; defensive */
    }
}

static void render_current_state(void)
{
    const michi_state_t st = michi_state_get();

    /* BOOTING/SELF_TEST are covered by the BSP boot screen (app_main renders
     * it before the boot events): never draw over it. */
    if (st == MICHI_STATE_BOOTING || st == MICHI_STATE_SELF_TEST) {
        return;
    }

    esp_err_t err = michi_board_display_render(render_frame);
    if (err == ESP_ERR_INVALID_STATE) {
        /* First render deferred until board init: the panel is not
         * available yet (app_main shows the BSP boot screen instead); the
         * next event re-renders, so a silent skip is correct - no bogus
         * "display degraded" warning on a healthy boot. */
        ESP_LOGD(TAG, "display: render_deferred reason=panel_unavailable");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display: render_failed err=%s", esp_err_to_name(err));
    }
}

static void display_task(void *arg)
{
    uint32_t msg;

    /* Initial render of the current state (covers late init; at boot the
     * state is BOOTING so this is a no-op). */
    render_current_state();

    for (;;) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (msg != MICHI_DISPLAY_MSG_RENDER) {
            ESP_LOGW(TAG, "display: unknown_cmd=%u", (unsigned)msg);
            continue;
        }
        /* Coalesce: drain pending requests, then render once with the latest
         * state - a state/redraw storm must not stack ~80 ms flushes. */
        while (xQueueReceive(s_queue, &msg, 0) == pdTRUE) {
            /* discard */
        }
        render_current_state();

        /* A request dropped while the queue was full would leave the screen
         * stale: re-render once when a producer signalled a drop. */
        if (s_pending) {
            s_pending = false;
            render_current_state();
        }
    }
}

esp_err_t michi_display_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_queue = xQueueCreate(CONFIG_MICHI_DISPLAY_QUEUE_LEN, sizeof(uint32_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "init: queue allocation failed");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t rc = xTaskCreate(display_task, "michi_display",
                                CONFIG_MICHI_DISPLAY_TASK_STACK_BYTES, NULL,
                                MICHI_DISPLAY_TASK_PRIORITY, &s_task);
    if (rc != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        ESP_LOGE(TAG, "init: task creation failed");
        return ESP_ERR_NO_MEM;
    }

    /* Filter 0 = all events: the observer reacts only to STATE_CHANGED and
     * ERROR, and only enqueues (it never renders nor blocks). */
    esp_err_t err = michi_state_register_observer(0, on_state_event);
    if (err != ESP_OK) {
        vTaskDelete(s_task);
        s_task = NULL;
        vQueueDelete(s_queue);
        s_queue = NULL;
        ESP_LOGE(TAG, "init: observer registration failed (%s)",
                 esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "subsystem=display state=ok phase=6");
    return ESP_OK;
}

esp_err_t michi_display_update_now_playing(const char *source,
                                           const char *title,
                                           const char *artist)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Bounded copies under a short lock: no formatting inside the critical
     * section (strnlen + memcpy is fixed cost, truncation semantics match
     * snprintf("%s") into the internal buffers). */
    portENTER_CRITICAL(&s_info_mux);
    copy_bounded(s_source, sizeof(s_source), source);
    copy_bounded(s_title, sizeof(s_title), title);
    copy_bounded(s_artist, sizeof(s_artist), artist);
    portEXIT_CRITICAL(&s_info_mux);

    queue_render();
    return ESP_OK;
}

void michi_display_request_redraw(void)
{
    if (!s_initialized) {
        return;
    }
    queue_render();
}

esp_err_t michi_display_clear_now_playing(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_info_mux);
    s_source[0] = '\0';
    s_title[0] = '\0';
    s_artist[0] = '\0';
    portEXIT_CRITICAL(&s_info_mux);
    queue_render();
    return ESP_OK;
}

esp_err_t michi_display_show_pairing_pin(const char *pin)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_info_mux);
    if (pin != NULL) {
        /* The buffer is exactly 6 digits + NUL: a malformed string
         * renders as "--" (never overflow, never truncation). */
        bool digits = strlen(pin) == 6;
        for (size_t i = 0; i < 6; i++) {
            digits = digits && pin[i] >= '0' && pin[i] <= '9';
        }
        if (digits) {
            memcpy(s_pairing_pin, pin, 7);
        } else {
            s_pairing_pin[0] = '\0';
        }
    } else {
        s_pairing_pin[0] = '\0';
    }
    portEXIT_CRITICAL(&s_info_mux);
    queue_render();
    return ESP_OK;
}

esp_err_t michi_display_clear_pairing_pin(void)
{
    return michi_display_show_pairing_pin(NULL);
}
