#pragma once
/* H15: RTP receive/PCM admission decisions, shared between the real worker
 * and the zero-device host contract test. Pure C11, no FreeRTOS dependencies.
 * The worker MUST evaluate this AFTER recvfrom(), not before its wait. */
#include <stdbool.h>

typedef enum {
    MICHI_AUDIO_WORKER_STOP = 0,
    MICHI_AUDIO_WORKER_PAUSE,
    MICHI_AUDIO_WORKER_PLAY,
} michi_audio_worker_action_t;

static inline michi_audio_worker_action_t
michi_audio_worker_next_action(bool running, bool paused)
{
    if (!running) return MICHI_AUDIO_WORKER_STOP;
    if (paused) return MICHI_AUDIO_WORKER_PAUSE;
    return MICHI_AUDIO_WORKER_PLAY;
}

/* A PCM producer rejected by QUIESCE is NOT a terminal socket failure.
 * A genuine I2S fault must still terminate the session fail-closed. */
static inline bool michi_audio_pause_interrupted_pcm(bool running,
                                                     bool paused,
                                                     bool output_quiesced)
{
    return running && paused && output_quiesced;
}
