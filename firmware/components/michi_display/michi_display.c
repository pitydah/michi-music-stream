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
#include "michi_ota.h"
#include "michi_product_profile.h"
#include "michi_session.h"
#include "michi_state.h"
#include "michi_ui.h"
#include "michi_volume.h"
#include "michi_wifi.h"

#define TAG "michi_display"

/* Render task priority: below the FSM task (5) so the event bus is never
 * starved, and below the I2S consumer (8, phase 11) so a frame flush
 * (8 sequential band flushes over a small DMA framebuffer, MS-11) never
 * delays audio; above app_main (1). */
#define MICHI_DISPLAY_TASK_PRIORITY 4

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

/* Bounded NUL-terminated copy for the now-playing buffers: strnlen + memcpy
 * keeps the critical section at fixed cost (no formatting under the lock).
 * NULL/empty input renders as empty string downstream. */
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

/* Draw callback for michi_board_display_render(): renders the screen
 * matching the CURRENT state for ONE band via michi_ui_render_screen().
 * All layout rows stay absolute; band intersection and per-pixel clipping
 * happen inside the UI primitives (MS-11 band contract).
 * Runs on the render task only. */
static void render_frame(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                         uint16_t y_origin)
{
    const michi_product_profile_t *p = michi_product_profile_get();

    char src[MICHI_DISPLAY_SOURCE_MAX + 1];
    char title[MICHI_DISPLAY_TITLE_MAX + 1];
    char artist[MICHI_DISPLAY_ARTIST_MAX + 1];
    char pin[7];
    uint32_t last_err;
    int8_t rssi;

    portENTER_CRITICAL(&s_info_mux);
    copy_bounded(src, sizeof(src), s_source);
    copy_bounded(title, sizeof(title), s_title);
    copy_bounded(artist, sizeof(artist), s_artist);
    copy_bounded(pin, sizeof(pin), s_pairing_pin);
    last_err = s_last_error;
    portEXIT_CRITICAL(&s_info_mux);

    michi_ui_screen_ctx_t ctx = {
        .state = michi_state_get(),
        .title = title,
        .artist = artist,
        .source = src,
        .pairing_pin = pin,
        .volume = michi_volume_get(),
        .sample_rate = p != NULL ? p->validated_sample_rate : 48000u,
        .bit_depth = MICHI_DISPLAY_APPLIED_BIT_DEPTH,
        .last_error = last_err,
        /* Live status reads (phase 12): the header indicators and the
         * "Actualizando" progress bar show REAL values, never literals.
         * All four calls are safe pre-init and cheap (no blocking on the
         * audio pipeline):
         *  - michi_wifi_is_provisioned(): reads a cached flag under a
         *    portMUX; before wifi init it returns false (provisioned +
         *    link up = connected; any error = false).
         *  - michi_wifi_get_rssi(): one esp_wifi_sta_get_ap_info() driver
         *    call; ESP_ERR_INVALID_STATE pre-init, ESP_ERR_NOT_FOUND when
         *    the STA has no AP. The display task (prio 4, below the I2S
         *    consumer at 8) never blocks the audio pipeline.
         *  - michi_session_active(): false pre-init; reconciles the lease
         *    (session alive = server connected - the only honest signal).
         *  - michi_ota_get_state(): reads state + percent under the OTA
         *    mutex; pre-init it returns ESP_OK with IDLE/0. Only read
         *    while the FSM is in UPDATING.
         * Assumption: the first frame after boot runs while the FSM is in
         * BOOTING/IDLE (wifi/session/ota already initialized by app_main
         * before the boot events), so pre-init paths are defensive only. */
        .wifi_connected = (michi_wifi_is_provisioned() &&
                           michi_wifi_get_rssi(&rssi) == ESP_OK),
        .server_connected = michi_session_active(),
        .update_pct = 0,
        .show_diagnostics = false,
    };

    /* Real update progress: only meaningful while the FSM is in UPDATING.
     * Re-read the state to avoid a stale ctx if the FSM advanced between
     * the ctx build and here (michi_state_get() is a cheap read). */
    if (ctx.state == MICHI_STATE_UPDATING) {
        michi_ota_state_t ota_state;
        int ota_pct = 0;
        if (michi_ota_get_state(&ota_state, &ota_pct, NULL, 0) == ESP_OK &&
            ota_pct >= 0 && ota_pct <= 100) {
            ctx.update_pct = (uint8_t)ota_pct;
        }
    }

    michi_ui_render_screen(fb, fb_w, fb_h, y_origin, &ctx);
}

static void render_current_state(void)
{
    /* BOOTING/SELF_TEST are drawn HERE by the render task using the
     * product boot screen (michi_ui_draw_screen_boot via
     * michi_ui_render_screen): app_main triggers an early redraw after
     * board init (michi_display_request_redraw) so the panel shows the
     * boot screen while the rest of the boot continues. The BSP legacy
     * boot screen is out of the normal flow (its technical content lives
     * in the logs and on the diagnostics screen). */

    esp_err_t err = michi_board_display_render(render_frame);
    if (err == ESP_ERR_INVALID_STATE) {
        /* First render deferred until board init: the panel is not
         * available yet (before board_init the render task skips
         * silently - the panel stays black until app_main triggers the
         * early redraw); the next event re-renders, so a silent skip is
         * correct - no bogus "display degraded" warning on a healthy
         * boot. */
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
     * state is BOOTING, so this draws the product boot screen as soon as
     * the panel is available - before that it skips silently). */
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
