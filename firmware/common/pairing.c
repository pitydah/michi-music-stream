#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "pairing.h"

static const char *TAG = "michi_pairing";

typedef struct { char id[32]; char token[64]; } controller_t;

static bool s_win = false;
static int64_t s_expires = 0;
static char s_nonce[32] = {0};
static controller_t s_ctrl[MAX_PAIRED_CONTROLLERS];
static int s_count = 0;
static pairing_state_cb_t s_cb = NULL;

static void gen_nonce(char *out, size_t sz)
{
    static const char *c = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (size_t i = 0; i < sz - 1; i++) out[i] = c[rand() % 36];
    out[sz - 1] = 0;
}

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open("michi_pairing", NVS_READONLY, &h) != ESP_OK) return;
    int8_t n = 0;
    if (nvs_get_i8(h, "count", &n) == ESP_OK) {
        s_count = n;
        for (int i = 0; i < n && i < MAX_PAIRED_CONTROLLERS; i++) {
            char k1[24], k2[24];
            snprintf(k1, sizeof(k1), "id_%d", i); snprintf(k2, sizeof(k2), "tok_%d", i);
            size_t l;
            l = sizeof(s_ctrl[i].id); nvs_get_str(h, k1, s_ctrl[i].id, &l);
            l = sizeof(s_ctrl[i].token); nvs_get_str(h, k2, s_ctrl[i].token, &l);
        }
    }
    nvs_close(h);
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open("michi_pairing", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i8(h, "count", (int8_t)s_count);
    for (int i = 0; i < s_count && i < MAX_PAIRED_CONTROLLERS; i++) {
        char k1[24], k2[24];
        snprintf(k1, sizeof(k1), "id_%d", i); snprintf(k2, sizeof(k2), "tok_%d", i);
        nvs_set_str(h, k1, s_ctrl[i].id); nvs_set_str(h, k2, s_ctrl[i].token);
    }
    nvs_commit(h);
    nvs_close(h);
}

void pairing_init(void)
{
    srand(time(NULL)); load();
    ESP_LOGI(TAG, "%d controller(s) paired", s_count);
}

void pairing_set_state_callback(pairing_state_cb_t cb)
{
    s_cb = cb;
}

bool pairing_start(const char *id, char *nonce_out, size_t nsz)
{
    if (s_win) return false;
    s_win = true;
    s_expires = esp_timer_get_time() / 1000 + PAIRING_WINDOW_SECONDS * 1000;
    gen_nonce(s_nonce, sizeof(s_nonce));
    if (nonce_out && nsz > 0) strncpy(nonce_out, s_nonce, nsz);
    if (s_cb) s_cb(true, id);
    ESP_LOGI(TAG, "Window open %d s nonce=%s", PAIRING_WINDOW_SECONDS, s_nonce);
    return true;
}

bool pairing_confirm(const char *nonce, const char *id, const char *token)
{
    if (!s_win) return false;
    if (strncmp(nonce, s_nonce, sizeof(s_nonce)) != 0) return false;
    for (int i = 0; i < s_count; i++)
        if (strcmp(s_ctrl[i].id, id) == 0) {
            strncpy(s_ctrl[i].token, token, sizeof(s_ctrl[i].token) - 1);
            save(); s_win = false; return true;
        }
    if (s_count >= MAX_PAIRED_CONTROLLERS) return false;
    strncpy(s_ctrl[s_count].id, id, sizeof(s_ctrl[s_count].id) - 1);
    strncpy(s_ctrl[s_count].token, token, sizeof(s_ctrl[s_count].token) - 1);
    s_count++; save(); s_win = false;
    memset(s_nonce, 0, sizeof(s_nonce));
    if (s_cb) s_cb(false, NULL);
    ESP_LOGI(TAG, "Paired with %s", id);
    return true;
}

bool pairing_is_window_open(void)
{
    if (s_win && (esp_timer_get_time() / 1000) >= s_expires) {
        s_win = false; memset(s_nonce, 0, sizeof(s_nonce));
        ESP_LOGI(TAG, "Window expired");
    }
    return s_win;
}

bool pairing_is_paired(void) { return s_count > 0; }

bool pairing_validate_token(const char *token)
{
    if (!token) return false;
    for (int i = 0; i < s_count; i++)
        if (strcmp(s_ctrl[i].token, token) == 0) return true;
    return false;
}

void pairing_factory_reset(void)
{
    s_count = 0; memset(s_ctrl, 0, sizeof(s_ctrl));
    s_win = false; memset(s_nonce, 0, sizeof(s_nonce));
    nvs_handle_t h;
    if (nvs_open("michi_pairing", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h); nvs_commit(h); nvs_close(h);
    }
    ESP_LOGW(TAG, "Factory reset");
}

void pairing_button_pressed(void) { pairing_start("button", NULL, 0); }
