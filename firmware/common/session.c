#include <string.h>
#include "esp_log.h"
#include "session.h"
#include "heartbeat.h"
#include "volume.h"
#include "audio_output.h"
#include "config.h"

static const char *TAG = "michi_session";

typedef struct { char id[32]; char codec[16]; int sr, bd, ch, sp, bm, vol; bool active; } sess_t;
static sess_t s;

void session_init(void) { memset(&s, 0, sizeof(s)); }

static bool codec_ok(const char *c)
{
    const char *ok[] = SUPPORTED_CODECS;
    for (int i = 0; i < sizeof(ok)/sizeof(ok[0]); i++)
        if (strcmp(c, ok[i]) == 0) return true;
    return false;
}

bool session_start(const char *sid, const char *codec, int sr, int bd, int ch, int sp, int bm, int vol)
{
    if (s.active) return false;
    if (!codec_ok(codec)) return false;
    if (sr > MAX_SAMPLE_RATE || bd > MAX_BIT_DEPTH) return false;
    if (ch < 1 || ch > 2 || bm < 50 || bm > 2000) return false;
    strncpy(s.id, sid, sizeof(s.id)-1); strncpy(s.codec, codec, sizeof(s.codec)-1);
    s.sr = sr; s.bd = bd; s.ch = ch; s.sp = sp; s.bm = bm; s.vol = vol; s.active = true;
    volume_set(vol);
    audio_output_init(sr, bd, ch, bm, sp);
    audio_output_start();
    heartbeat_reset();
    ESP_LOGI(TAG, "Session %s started", sid);
    return true;
}

bool session_stop(void)
{
    if (!s.active) return false;
    audio_output_stop(); heartbeat_stop();
    memset(&s, 0, sizeof(s));
    return true;
}

bool session_is_active(void)
{
    if (!s.active) return false;
    if (!heartbeat_is_active()) { session_stop(); return false; }
    return true;
}
