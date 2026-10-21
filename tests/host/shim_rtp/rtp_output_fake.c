/* H18: test doubles for hardware-only dependencies; RTP engine itself is real. */
#include <stdatomic.h>
#include <string.h>
#include "michi_audio_output.h"
#include "michi_board.h"
#include "michi_dac.h"
#include "michi_product_profile.h"
#include "michi_state.h"
#include "michi_volume.h"
#include "rtp_output_fake.h"

static _Atomic int s_state = MICHI_AUDIO_STATE_RUNNING;
static _Atomic uint64_t s_written = 0;
static _Atomic uint32_t s_rejected = 0;
static _Atomic bool s_quiesce_fail = false;

void test_rtp_output_reset(void) {
    s_state = MICHI_AUDIO_STATE_RUNNING;
    s_written = 0;
    s_rejected = 0;
    s_quiesce_fail = false;
}
uint64_t test_rtp_output_written_bytes(void) { return s_written; }
uint32_t test_rtp_output_rejected_writes(void) { return s_rejected; }
void test_rtp_output_force_quiesce_fail(bool failed) { s_quiesce_fail = failed; }

michi_audio_output_state_t michi_audio_output_get_state(void) {
    return (michi_audio_output_state_t)atomic_load(&s_state);
}
bool michi_audio_output_is_running(void) {
    return s_state == MICHI_AUDIO_STATE_RUNNING || s_state == MICHI_AUDIO_STATE_QUIESCED;
}
bool michi_audio_output_is_quiesced(void) { return s_state == MICHI_AUDIO_STATE_QUIESCED; }
esp_err_t michi_audio_output_init(const michi_audio_output_config_t *cfg) {
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s_state = MICHI_AUDIO_STATE_INITIALIZED;
    return ESP_OK;
}
esp_err_t michi_audio_output_start(void) {
    s_state = MICHI_AUDIO_STATE_RUNNING;
    return ESP_OK;
}
esp_err_t michi_audio_output_stop(void) {
    s_state = MICHI_AUDIO_STATE_STOPPED;
    return ESP_OK;
}
esp_err_t michi_audio_output_deinit(void) {
    s_state = MICHI_AUDIO_STATE_UNINITIALIZED;
    return ESP_OK;
}
esp_err_t michi_audio_output_quiesce(void) {
    if (s_quiesce_fail) { s_state = MICHI_AUDIO_STATE_FAULTED; return ESP_FAIL; }
    if (!michi_audio_output_is_running()) return ESP_ERR_INVALID_STATE;
    s_state = MICHI_AUDIO_STATE_QUIESCED;
    return ESP_OK;
}
esp_err_t michi_audio_output_resume(void) {
    if (s_state != MICHI_AUDIO_STATE_QUIESCED) return ESP_ERR_INVALID_STATE;
    s_state = MICHI_AUDIO_STATE_RUNNING;
    return ESP_OK;
}
esp_err_t michi_audio_output_write(const uint8_t *data, size_t len) {
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    if (s_state != MICHI_AUDIO_STATE_RUNNING) {
        atomic_fetch_add(&s_rejected, 1);
        return ESP_ERR_INVALID_STATE;
    }
    atomic_fetch_add(&s_written, (uint64_t)len);
    return ESP_OK;
}

static const michi_dac_caps_t s_dac = {.detected = true, .initialized = true};
const michi_dac_caps_t *michi_dac_get_caps(void) { return &s_dac; }
esp_err_t michi_dac_start(uint32_t r, uint8_t b, uint8_t c) {
    (void)r; (void)b; (void)c;
    return ESP_OK;
}
static const michi_board_external_pins_t s_pins = {
    .i2s_bclk=3, .i2s_lrck=18, .i2s_din=5, .i2s_mclk=-1
};
const michi_board_external_pins_t *michi_board_get_external_pins(void) { return &s_pins; }
esp_err_t michi_product_profile_refresh(void) { return ESP_OK; }
const char *michi_product_profile_tier_name(void) { return "standard"; }
esp_err_t michi_state_report_error(michi_event_id_t event, uint32_t value) {
    (void)event; (void)value;
    return ESP_OK;
}
esp_err_t michi_volume_set(uint8_t volume) { (void)volume; return ESP_OK; }
void michi_volume_set_muted(bool muted) { (void)muted; }
