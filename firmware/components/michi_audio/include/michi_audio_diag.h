#pragma once
/* H19: truthful software-only receiver observations.
 * This does NOT claim that the DAC's analog output is audible, that any DMA
 * queue is empty, or that the peer is currently sending useful audio.
 * The response is a non-atomic diagnostic observation across subsystems. */
#include <stdbool.h>
#include <stdint.h>

static inline const char *michi_audio_observation_stage(
    bool session_present, bool rtp_worker_active, bool paused,
    bool i2s_faulted, uint32_t packets_received, uint32_t pcm_packets_submitted)
{
    if (i2s_faulted) return "i2s_faulted";
    if (!session_present || !rtp_worker_active) return "inactive";
    if (paused) return "paused";
    if (packets_received == 0) return "waiting_for_rtp";
    if (pcm_packets_submitted == 0) return "buffering";
    return "pcm_submitted";
}
