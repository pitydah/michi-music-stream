#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <stdbool.h>

void audio_output_init(int sample_rate, int bit_depth, int channels, int buffer_ms, int stream_port);
void audio_output_start(void);
void audio_output_stop(void);
bool audio_output_is_running(void);

#endif
