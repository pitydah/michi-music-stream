/* H19: deterministic classification of software-only audio observations. */
#include <stdio.h>
#include <string.h>
#include "michi_audio_diag.h"

static int failed;
static void assert_stage(const char *expected, bool present, bool engine,
                         bool paused, bool faulted, unsigned received,
                         unsigned submitted)
{
    const char *stage = michi_audio_observation_stage(
        present, engine, paused, faulted, received, submitted);
    if (strcmp(stage, expected) != 0) {
        fprintf(stderr, "diagnostic stage expected=%s got=%s\n", expected, stage);
        failed++;
    }
}

int main(void)
{
    assert_stage("inactive", false, false, false, false, 0, 0);
    assert_stage("inactive", true, false, false, false, 10, 8);
    assert_stage("waiting_for_rtp", true, true, false, false, 0, 0);
    assert_stage("buffering", true, true, false, false, 4, 0);
    assert_stage("pcm_submitted", true, true, false, false, 4, 2);
    assert_stage("paused", true, true, true, false, 4, 2);
    assert_stage("i2s_faulted", true, true, true, true, 4, 2);
    assert_stage("i2s_faulted", false, false, false, true, 0, 0);
    if (failed) return 1;
    puts("H19 software audio observation classification: PASS");
    return 0;
}
