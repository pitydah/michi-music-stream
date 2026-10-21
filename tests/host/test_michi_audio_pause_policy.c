/* H15: actual RTP worker policy header compiled as part of host suite.
 * Contract-only unit tests complement (DO NOT replace) device/engine E2E. */
#include <stdio.h>
#include "michi_audio_pause_policy.h"

#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr, "H15 pause policy FAILED line=%d\n", __LINE__); \
    return 1; } } while (0)

int main(void)
{
    /* PAUSE can flip during a blocking recvfrom(): only the state read
     * AFTER the wait should determine whether the worker writes PCM. */
    REQUIRE(michi_audio_worker_next_action(true, false) == MICHI_AUDIO_WORKER_PLAY);
    REQUIRE(michi_audio_worker_next_action(true, true) == MICHI_AUDIO_WORKER_PAUSE);
    REQUIRE(michi_audio_worker_next_action(false, false) == MICHI_AUDIO_WORKER_STOP);
    REQUIRE(michi_audio_worker_next_action(false, true) == MICHI_AUDIO_WORKER_STOP);
    REQUIRE(michi_audio_pause_interrupted_pcm(true, true, true));
    REQUIRE(!michi_audio_pause_interrupted_pcm(true, false, true));
    REQUIRE(!michi_audio_pause_interrupted_pcm(true, true, false));
    REQUIRE(!michi_audio_pause_interrupted_pcm(false, true, true));
    puts("michi_audio_pause_policy: all scenarios passed");
    return 0;
}
