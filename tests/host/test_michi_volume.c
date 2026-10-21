/* H7 volume/mute regression suite: compiles REAL michi_volume.c. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "michi_dac.h"
#include "michi_volume.h"

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

static michi_dac_caps_t s_caps;
static uint8_t s_hw_level;
static unsigned s_hw_writes;
static bool s_hw_fail;

const michi_dac_caps_t *michi_dac_get_caps(void)
{
    return &s_caps;
}

esp_err_t michi_dac_set_volume(uint8_t requested)
{
    s_hw_writes++;
    if (s_hw_fail) {
        return ESP_FAIL;
    }
    s_hw_level = requested;
    return ESP_OK;
}

static void reset_volume(bool hardware)
{
    memset(&s_caps, 0, sizeof(s_caps));
    s_caps.initialized = true;
    s_caps.hardware_volume = hardware;
    s_hw_fail = false;
    s_hw_writes = 0;
    s_hw_level = 100;
    CHECK(michi_volume_init() == ESP_OK, "volume init");
    CHECK(michi_volume_configure_pcm(48000, 1) == ESP_OK, "mono stream format");
    CHECK(!michi_volume_is_muted(), "newly initialized audio is unmuted");
}

/* H8: drain the 10-ms PCM envelope to test its settled value. */
static int16_t processed_sample(int16_t sample)
{
    uint8_t pcm[2];
    for (int i = 0; i < 480; ++i) {
        memcpy(pcm, &sample, sizeof(sample));
        michi_volume_apply(pcm, sizeof(pcm), 16);
    }
    memcpy(&sample, pcm, sizeof(sample));
    return sample;
}

static void fill_stereo(int16_t *pcm, size_t frames, int16_t sample)
{
    for (size_t i = 0; i < frames; ++i) {
        pcm[2 * i] = sample;
        pcm[2 * i + 1] = (int16_t)-sample;
    }
}

static void test_h8_stereo_fade_and_retarget(void)
{
    reset_volume(false);
    CHECK(michi_volume_set(100) == ESP_OK, "full volume before fade");
    CHECK(michi_volume_configure_pcm(48000, 2) == ESP_OK, "stereo format");
    int16_t pcm[480 * 2];
    fill_stereo(pcm, 480, 16000);
    michi_volume_apply((uint8_t *)pcm, sizeof(pcm), 16);
    CHECK(pcm[0] > 0 && pcm[0] < 16000, "first frame fades from silence");
    CHECK(pcm[1] == -pcm[0], "left and right share the same frame gain");
    CHECK(pcm[958] == 16000 && pcm[959] == -16000,
          "48 kHz ramp converges at 480 frames");
    for (size_t i = 2; i < 480 * 2; i += 2) {
        CHECK(pcm[i] >= pcm[i - 2], "gain monotonically increases");
        CHECK(pcm[i + 1] == -pcm[i], "same gain for both channels");
    }

    michi_volume_set_muted(true);
    fill_stereo(pcm, 480, 16000);
    michi_volume_apply((uint8_t *)pcm, sizeof(pcm), 16);
    CHECK(pcm[0] > 0 && pcm[0] < 16000, "mute fades rather than hard-gating");
    CHECK(pcm[958] == 0 && pcm[959] == 0, "mute reaches exact silence");
    CHECK(michi_volume_get() == 100, "mute does not change configured level");

    michi_volume_set_muted(false);
    fill_stereo(pcm, 480, 16000);
    michi_volume_apply((uint8_t *)pcm, sizeof(pcm), 16);
    CHECK(pcm[0] > 0 && pcm[0] < 16000, "unmute fades from silence");
    CHECK(pcm[958] == 16000, "unmute recovers full output");

    CHECK(michi_volume_set(30) == ESP_OK, "digital volume change");
    fill_stereo(pcm, 480, 16000);
    michi_volume_apply((uint8_t *)pcm, sizeof(pcm), 16);
    CHECK(pcm[0] < 16000 && pcm[0] > 4800, "volume decrease is ramped");
    CHECK(pcm[958] >= 4700 && pcm[958] <= 4800, "digital ramp settles at 30 percent");
}

static void test_h9_mid_fade_retarget_and_mute(void)
{
    reset_volume(false);
    CHECK(michi_volume_set(100) == ESP_OK, "H9 initial max volume");
    int16_t frames[240];
    for (unsigned i = 0; i < 240; ++i) frames[i] = 12000;
    michi_volume_apply((uint8_t *)frames, sizeof(frames), 16);
    const int16_t middle = frames[239];
    CHECK(middle >= 5800 && middle <= 6200, "H9 halfway through fade-in");
    michi_volume_set_muted(true);
    int16_t frame = 12000;
    michi_volume_apply((uint8_t *)&frame, sizeof(frame), 16);
    CHECK(frame <= middle && frame >= 0,
          "H9 mute retarget does not jump above current gain");
    for (unsigned i = 0; i < 480; ++i) {
        frame = 12000;
        michi_volume_apply((uint8_t *)&frame, sizeof(frame), 16);
    }
    CHECK(frame == 0, "H9 retarget eventually reaches exact silence");
}

static void test_h8_sample_rate_and_24_bit(void)
{
    reset_volume(false);
    CHECK(michi_volume_configure_pcm(96000, 1) == ESP_OK, "96 kHz allowed output rate");
    CHECK(michi_volume_set(100) == ESP_OK, "full gain");
    int16_t pcm[480];
    for (size_t i = 0; i < 480; ++i) pcm[i] = 10000;
    michi_volume_apply((uint8_t *)pcm, sizeof(pcm), 16);
    CHECK(pcm[479] >= 4900 && pcm[479] <= 5100,
          "fade is 10 ms in real frames, not a fixed 480 for every sample rate");
    for (size_t i = 0; i < 480; ++i) pcm[i] = 10000;
    michi_volume_apply((uint8_t *)pcm, sizeof(pcm), 16);
    CHECK(pcm[479] == 10000, "96 kHz fade ends at 960 frames");

    reset_volume(false);
    CHECK(michi_volume_set(50) == ESP_OK, "24bit half volume");
    CHECK(michi_volume_configure_pcm(48000, 1) == ESP_OK, "24bit mono format");
    uint8_t packed[3 * 480];
    for (size_t i = 0; i < 480; ++i) {
        packed[3 * i] = 0; packed[3 * i + 1] = 0; packed[3 * i + 2] = 0xC0;
    } /* -4194304 as LE signed 24-bit */
    michi_volume_apply(packed, sizeof(packed), 24);
    const size_t last = 3 * 479;
    int32_t v = (int32_t)((uint32_t)packed[last] |
                          (uint32_t)packed[last + 1] << 8 |
                          (uint32_t)packed[last + 2] << 16);
    if (v & 0x800000) v -= 0x1000000;
    CHECK(v == -2097152, "signed 24-bit PCM reaches half gain with correct sign");

    CHECK(michi_volume_configure_pcm(0, 2) == ESP_ERR_INVALID_ARG,
          "invalid sample rate rejected");
    CHECK(michi_volume_configure_pcm(48000, 3) == ESP_ERR_INVALID_ARG,
          "invalid channel count rejected");
}

static void test_software_mute_preserves_level(void)
{
    reset_volume(false);
    CHECK(michi_volume_set(65) == ESP_OK, "digital volume set");
    const int16_t reduced = processed_sample(10000);
    CHECK(reduced >= 6400 && reduced <= 6600, "volume scales PCM");

    michi_volume_set_muted(true);
    CHECK(michi_volume_is_muted(), "muted reported");
    CHECK(michi_volume_get() == 65, "mute does not destroy volume");
    CHECK(processed_sample(10000) == 0, "muted PCM is silent");
    CHECK(processed_sample(-10000) == 0, "negative muted PCM is silent");

    CHECK(michi_volume_set(35) == ESP_OK, "volume can change during mute");
    CHECK(michi_volume_get() == 35, "latest configured volume preserved");
    CHECK(processed_sample(10000) == 0, "changing volume does not unmute");

    michi_volume_set_muted(false);
    CHECK(!michi_volume_is_muted(), "unmuted reported");
    const int16_t restored = processed_sample(10000);
    CHECK(restored >= 3400 && restored <= 3600,
          "unmute restores latest configured gain instead of remaining at zero");
}

static void test_hardware_mute_is_a_pcm_gate(void)
{
    reset_volume(true);
    CHECK(michi_volume_set(42) == ESP_OK, "hardware volume change");
    CHECK(s_hw_level == 42, "DAC received requested hardware level");
    CHECK(processed_sample(10000) == 10000, "hardware path bypasses digital scaling");
    const unsigned writes = s_hw_writes;

    michi_volume_set_muted(true);
    CHECK(processed_sample(10000) == 0,
          "mute gates PCM even when hardware volume is active");
    CHECK(s_hw_writes == writes && s_hw_level == 42,
          "mute does not overwrite DAC level");
    michi_volume_set_muted(false);
    CHECK(processed_sample(10000) == 10000, "unmute preserves hardware gain path");
    CHECK(s_hw_level == 42, "DAC volume preserved after unmute");
}

static void test_failed_hardware_write_falls_back_to_digital(void)
{
    reset_volume(true);
    s_hw_fail = true;
    CHECK(michi_volume_set(25) == ESP_OK, "hardware failure has digital fallback");
    const int16_t sample = processed_sample(10000);
    CHECK(sample >= 2400 && sample <= 2600, "digital fallback scales PCM");
    michi_volume_set_muted(true);
    CHECK(processed_sample(10000) == 0, "mute still works after hardware failure");
    michi_volume_set_muted(false);
    CHECK(processed_sample(10000) == sample, "unmute retains fallback gain");
}

static void test_zero_level_and_defensive_buffer(void)
{
    reset_volume(false);
    CHECK(michi_volume_set(0) == ESP_OK, "zero-volume set");
    CHECK(processed_sample(10000) == 0, "volume zero produces silence");
    CHECK(michi_volume_set(255) == ESP_OK && michi_volume_get() == 100,
          "volume clamps to 100");
    michi_volume_apply(NULL, 0, 16); /* must not dereference */
    michi_volume_set_muted(true);
    /* H8 regression fix: mute is a 10-ms ramp, not instantaneous. Drain
     * 480 mono frames before asserting an all-zero complete sample. */
    for (unsigned i = 0; i < 480; ++i) {
        uint8_t frame[3] = {1, 2, 3};
        michi_volume_apply(frame, sizeof(frame), 24);
    }
    uint8_t arbitrary[5] = {1, 2, 3, 4, 5};
    michi_volume_apply(arbitrary, sizeof(arbitrary), 24);
    CHECK(memcmp(arbitrary, "\0\0\0\0\0", sizeof(arbitrary)) == 0,
          "settled mute silences complete samples and incomplete tails");
}

int main(void)
{
    test_software_mute_preserves_level();
    test_hardware_mute_is_a_pcm_gate();
    test_failed_hardware_write_falls_back_to_digital();
    test_zero_level_and_defensive_buffer();
    test_h8_stereo_fade_and_retarget();
    test_h8_sample_rate_and_24_bit();
    test_h9_mid_fade_retarget_and_mute();
    if (failures != 0) {
        fprintf(stderr, "michi_volume: %d FAILED checks\n", failures);
        return 1;
    }
    puts("michi_volume: all checks passed");
    return 0;
}
