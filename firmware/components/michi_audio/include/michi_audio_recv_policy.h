#pragma once
/* Portable receive-error classification for the Stream RTP socket.
 * Fail closed after three consecutive NON-transient errors. A standard
 * timeout (EAGAIN/EWOULDBLOCK) or EINTR is not a receiver failure.
 * No ESP-IDF dependency: validated by the host jitter-buffer test binary. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#define MICHI_AUDIO_RECV_FATAL_THRESHOLD 3u

static inline void michi_audio_recv_error_reset(uint8_t *streak)
{
    if (streak != NULL) {
        *streak = 0;
    }
}

static inline bool michi_audio_recv_error_is_fatal(uint8_t *streak, int error_code)
{
    if (streak == NULL) {
        return true;
    }
    if (error_code == EAGAIN || error_code == EWOULDBLOCK || error_code == EINTR) {
        michi_audio_recv_error_reset(streak);
        return false;
    }
    if (*streak < MICHI_AUDIO_RECV_FATAL_THRESHOLD) {
        ++*streak;
    }
    return *streak >= MICHI_AUDIO_RECV_FATAL_THRESHOLD;
}
