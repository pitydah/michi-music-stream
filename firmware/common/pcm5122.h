#ifndef PCM5122_H
#define PCM5122_H

#include <stdbool.h>

#define PCM5122_I2C_ADDR  0x4D

bool pcm5122_init(void);
void pcm5122_set_mute(bool mute);
void pcm5122_set_format(int sample_rate, int bit_depth);

#endif
