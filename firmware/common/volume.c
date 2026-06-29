#include <string.h>
#include "esp_log.h"
#include "volume.h"

static int s_vol = 100;

void volume_set(int v) { if (v < 0) v = 0; if (v > 100) v = 100; s_vol = v; }
int volume_get(void) { return s_vol; }

void volume_apply(void *buf, size_t n, int bd)
{
    float g = s_vol / 100.0f;
    if (g >= 1.0f) return;
    if (g <= 0.0f) { memset(buf, 0, n * (bd/8)); return; }
    if (bd == 16) {
        int16_t *p = buf;
        for (size_t i = 0; i < n; i++) p[i] = (int16_t)(p[i] * g);
    } else if (bd == 24) {
        uint8_t *b = buf;
        for (size_t i = 0; i < n; i++) {
            int32_t s = b[0] | (b[1]<<8) | (b[2]<<16);
            if (s & 0x800000) s |= 0xFF000000;
            s = (int32_t)(s * g);
            if (s > 8388607) s = 8388607; if (s < -8388608) s = -8388608;
            b[0] = s & 0xFF; b[1] = (s>>8) & 0xFF; b[2] = (s>>16) & 0xFF;
            b += 3;
        }
    }
}
