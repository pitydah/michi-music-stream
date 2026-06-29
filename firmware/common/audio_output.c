#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "audio_output.h"
#include "volume.h"
#include "config.h"

/*
 * audio_output.c — Pipeline de audio para Michi Music Stream.
 *
 * Limitaciones conocidas (v1-lite, fase inicial):
 * - Buffer circular simple: 4 MB, sin gestión avanzada de underrun/overrun.
 * - No hay PLL ni corrección de drift entre reloj del transmisor y receptor.
 * - Sin sincronización multiroom. La latencia no está calibrada.
 * - Bajo congestión Wi-Fi pueden ocurrir underruns (silencio insertado).
 * - Sin FEC ni re-transmisión de paquetes UDP perdidos.
 * - Sin decodificador Opus implementado (stub para pcm_s16le/pcm_s24le).
 *
 * El pipeline es funcional para pruebas con hardware real, pero NO es
 * apto para producción sin validación de jitter, drift y latencia.
 */

static const char *TAG = "michi_audio";

static int s_sr, s_bd, s_ch, s_bm, s_sp;
static bool s_run = false;
static TaskHandle_t s_ut = NULL, s_it = NULL;
static int s_sock = -1;

#define RB_SZ (4 * 1024 * 1024)
static uint8_t *s_rb = NULL;
static volatile size_t s_wp = 0, s_rp = 0, s_fill = 0;

static void rb_write(const uint8_t *d, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        s_rb[s_wp] = d[i];
        s_wp = (s_wp + 1) % RB_SZ;
        if (++s_fill > RB_SZ) { s_rp = (s_rp + 1) % RB_SZ; s_fill = RB_SZ; }
    }
}

static size_t rb_read(uint8_t *d, size_t len)
{
    size_t take = (len < s_fill) ? len : s_fill;
    for (size_t i = 0; i < take; i++) {
        d[i] = s_rb[s_rp];
        s_rp = (s_rp + 1) % RB_SZ;
        s_fill--;
    }
    return take;
}

static void udp_task(void *arg)
{
    struct sockaddr_in a = {
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_family = AF_INET,
        .sin_port = htons(s_sp)
    };
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock < 0) { ESP_LOGE(TAG, "socket failed"); vTaskDelete(NULL); return; }
    if (bind(s_sock, (struct sockaddr*)&a, sizeof(a)) < 0) {
        ESP_LOGE(TAG, "bind failed on port %d", s_sp);
        close(s_sock); s_sock = -1; vTaskDelete(NULL); return;
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = 50000};
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t *p = malloc(2048);
    if (!p) { close(s_sock); vTaskDelete(NULL); return; }
    ESP_LOGI(TAG, "UDP receiver listening on port %d", s_sp);
    while (s_run) {
        int l = recvfrom(s_sock, p, 2048, 0, NULL, NULL);
        if (l > 0) rb_write(p, l);
    }
    free(p); close(s_sock); s_sock = -1; vTaskDelete(NULL);
}

static void i2s_task(void *arg)
{
    size_t bps = (s_bd / 8) * s_ch;
    size_t pre  = (size_t)s_sr * bps * s_bm / 1000;

    ESP_LOGI(TAG, "Prefilling buffer: %u bytes (%d ms)", pre, s_bm);
    while (s_run && s_fill < pre) vTaskDelay(pdMS_TO_TICKS(10));

    i2s_chan_handle_t tx = NULL;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&cc, &tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %d", err);
        vTaskDelete(NULL); return;
    }

    i2s_std_config_t sc = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)s_sr,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            (i2s_data_bit_width_t)s_bd,
            (i2s_slot_mode_t)s_ch
        ),
        .gpio_cfg = {
            .mclk = (s_sr > 48000) ? I2S_MCLK_PIN : I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN,
            .ws   = I2S_LRC_PIN,
            .dout = I2S_DIN_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {0},
        },
    };
    err = i2s_channel_init_std_mode(tx, &sc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %d", err);
        i2s_del_channel(tx); vTaskDelete(NULL); return;
    }
    i2s_channel_enable(tx);

    uint8_t *b = malloc(1024);
    if (!b) { i2s_channel_disable(tx); i2s_del_channel(tx); vTaskDelete(NULL); return; }
    size_t bps2 = s_bd / 8;

    ESP_LOGI(TAG, "I2S playback started: %d Hz %d-bit %dch", s_sr, s_bd, s_ch);
    while (s_run) {
        size_t g = rb_read(b, 1024);
        if (g > 0) {
            volume_apply(b, g / bps2, s_bd);
            size_t w = 0;
            i2s_channel_write(tx, b, g, &w, portMAX_DELAY);
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    free(b); i2s_channel_disable(tx); i2s_del_channel(tx); vTaskDelete(NULL);
}

void audio_output_init(int sr, int bd, int ch, int bm, int sp)
{
    s_sr = sr; s_bd = bd; s_ch = ch; s_bm = bm; s_sp = sp;
    if (!s_rb) s_rb = malloc(RB_SZ);
    if (!s_rb) { ESP_LOGE(TAG, "ring buffer alloc failed"); return; }
    s_wp = s_rp = s_fill = 0;
    ESP_LOGI(TAG, "audio_output_init: %d Hz %d-bit %dch buf=%dms port=%d", sr, bd, ch, bm, sp);
}

void audio_output_start(void)
{
    if (s_run) return;
    s_run = true;
    xTaskCreate(udp_task, "udp_rx", 4096, NULL, 6, &s_ut);
    xTaskCreate(i2s_task, "i2s_tx", 4096, NULL, 6, &s_it);
}

void audio_output_stop(void)
{
    s_run = false;
    if (s_ut) { vTaskDelete(s_ut); s_ut = NULL; }
    if (s_it) { vTaskDelete(s_it); s_it = NULL; }
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
    ESP_LOGI(TAG, "audio_output stopped");
}

bool audio_output_is_running(void) { return s_run; }
