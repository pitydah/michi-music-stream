#include "michi_audio.h"

#define MICHI_AUDIO_SAMPLE_RATE 48000
#define MICHI_AUDIO_SAMPLES_PER_PACKET 480
#define MICHI_AUDIO_PACKET_MS ((uint32_t)((uint64_t)MICHI_AUDIO_SAMPLES_PER_PACKET * 1000 / MICHI_AUDIO_SAMPLE_RATE))

esp_err_t michi_audio_validate_buffer_ms(uint16_t buffer_ms)
{
    if (buffer_ms < MICHI_AUDIO_BUFFER_MS_MIN || buffer_ms > MICHI_AUDIO_BUFFER_MS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

uint32_t michi_audio_calculate_prefill_target_ext(uint16_t buffer_ms, uint32_t max_packets)
{
    uint32_t prefill_ms = (buffer_ms > 0) ? buffer_ms : 80;
    uint32_t packet_ms = MICHI_AUDIO_PACKET_MS;
    if (packet_ms == 0) {
        packet_ms = 1;
    }
    uint32_t target = (prefill_ms + packet_ms - 1) / packet_ms;
    return target > max_packets ? max_packets : target;
}

uint32_t michi_audio_calculate_prefill_target(uint16_t buffer_ms)
{
#ifdef CONFIG_MICHI_AUDIO_JITTER_MAX_MS
    const uint32_t max_packets = CONFIG_MICHI_AUDIO_JITTER_MAX_MS / 10;
#else
    const uint32_t max_packets = MICHI_AUDIO_BUFFER_MS_MAX / 10;
#endif
    return michi_audio_calculate_prefill_target_ext(buffer_ms, max_packets);
}

uint32_t michi_audio_recovery_deadline_ms(uint16_t buffer_ms)
{
    uint32_t ms = (buffer_ms > 0) ? buffer_ms : 80;
    /* Allow at least buffer_ms + 250 ms for natural real-time packet arrival */
    uint32_t deadline = ms + 250;
    if (deadline < 500) {
        deadline = 500;
    }
    if (deadline > 1000) {
        deadline = 1000;
    }
    return deadline;
}

esp_err_t michi_audio_check_capacity_invariant(uint32_t jitter_max_ms, uint16_t advertised_max_ms)
{
    if (jitter_max_ms < advertised_max_ms) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
