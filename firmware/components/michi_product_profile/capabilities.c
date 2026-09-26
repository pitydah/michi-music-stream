/*
 * Canonical product capability flags - THE single source of truth for the
 * boolean feature surface (MS-08 / P0-01 hardening).
 *
 * Both the discovery announce (michi_discovery) and GET /server/info
 * (michi_http canonical_json.c) read THIS table; no subsystem duplicates
 * a capability literal. A flag is true only while its handler is
 * implemented with a positive test:
 *  - session/heartbeat/volume: implemented (MS-07/MS-08);
 *  - diagnostics: implemented (its response shape is not frozen by the
 *    contract);
 *  - now_playing: the certified payload still answers 501 NOT_IMPLEMENTED;
 *  - ota: the A/B partitions exist, but the OTA service lands in phase 13
 *    (501).
 *
 * Four Distinct Capability Tiers (Signal Truth Architecture):
 * ------------------------------------------------------------
 * 1. Silicon Hardware Limits:
 *    Physical limits of the audio silicon (e.g. PCM5122 supports up to 384 kHz,
 *    16/24/32-bit slot audio data, 112 dB SNR, and hardware volume attenuation
 *    from -103.5 dB to +24 dB; PCM5102A supports up to 384 kHz / 32-bit but has
 *    no I2C volume control).
 * 2. Driver Implementation:
 *    ESP-IDF driver configuration and register control (e.g. I2S standard mode
 *    clocked for 48 kHz / 16-bit payload in 16-bit or 32-bit frame slots, and
 *    I2C register initialization/attenuation commands for supported DAC models).
 * 3. Validated System Capability:
 *    The end-to-end signal path verified and certified under test across RTOS
 *    tasks, jitter buffer, and audio engine: 48 kHz, 16-bit stereo PCM, 10 ms
 *    RTP packetization (480 frames / 960 samples / 1920 bytes per packet), and
 *    buffer depth of 50..500 ms.
 * 4. Wire Protocol / Advertised Capability:
 *    The external contract surface announced over mDNS discovery and HTTP
 *    GET /api/v1/receiver-lite/server_info. The wire protocol advertises ONLY what
 *    is certified end-to-end (48 kHz / 16-bit stereo PCM, canonical features
 *    {session, heartbeat, volume}). Higher silicon features (e.g. 96/192/384 kHz
 *    or 24-bit PCM) are explicitly gated and not advertised until verified.
 *
 * The announce carries ONLY the canonical group {heartbeat, session,
 * volume}; the extended flags (now_playing/diagnostics/ota) belong to
 * /server/info, not to the announce.
 *
 * Pure C (no ESP-IDF runtime dependency): the component and tests/host
 * compile the SAME source.
 */

#include "michi_product_profile.h"

static const michi_product_capabilities_t s_capabilities = {
    .session = true,
    .heartbeat = true,
    .volume = true,
    .now_playing = false,
    .diagnostics = true,
    .ota = false,
};

const michi_product_capabilities_t *michi_product_profile_capabilities(void)
{
    return &s_capabilities;
}

michi_product_capabilities_t michi_product_profile_capabilities_for(const michi_product_profile_t *p)
{
    michi_product_capabilities_t caps = s_capabilities;
    if (p != NULL && !p->audio_available) {
        /* Signal Truth: no session or volume can be accepted if audio pipeline is not available */
        caps.session = false;
        caps.heartbeat = false;
        caps.volume = false;
    }
    return caps;
}
