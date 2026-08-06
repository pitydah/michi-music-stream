#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief RTP/UDP audio engine (phase 11: meta 1).
 *
 * Receives a PCM S16LE RTP stream over UDP, absorbs network jitter with a
 * packet-level jitter buffer, and feeds the michi_audio_output pipeline
 * (which applies michi_volume_apply() on its write path). The unsynchronized
 * 4 MB global buffer of the legacy firmware is NOT used: resilience comes
 * from the bounded per-session jitter buffer (MICHI_AUDIO_JITTER_MAX_MS),
 * the SPSC ring of michi_audio_output and lwip's UDP receive mailbox
 * (CONFIG_LWIP_UDP_RECVMBOX_SIZE, raised in sdkconfig.defaults so bursts
 * land in the jitter buffer instead of the socket).
 *
 * ------------------------------------------------------------------
 * Phase-11 scope (meta 1) vs declared future metas
 * ------------------------------------------------------------------
 * META 1 (implemented here): PCM S16LE, 48 kHz, stereo; packet jitter
 * buffer; 80 ms prefill; sequence/timestamp/SSRC handling; duplicates,
 * reordering, late packets, loss; metrics; explicit silence on underrun and
 * gaps; cooperative shutdown. The validated profile is the phase-2/4
 * baseline: 48000/16/2 (michi_product_profile).
 *
 * The following metas are DECLARED but NOT implemented (rejected or
 * documented, never faked):
 *  - S24LE payloads (PT 96, dynamic): rejected with a log; the constant
 *    MICHI_AUDIO_RTP_PT_S24LE declares the mapping for the S24LE phase.
 *  - 44.1 kHz / 48-96 kHz per product profile: prepare() answers
 *    ESP_ERR_NOT_SUPPORTED outside 48000/16/2.
 *  - RTCP (RFC 3550 reception quality / sender reports): NOT implemented;
 *    jitter is estimated locally without RTCP and the playhead is NOT
 *    drift-corrected (see Timestamps below).
 *  - Clock drift correction: NOT implemented (requires RTCP NTP mapping);
 *    a stalled/skewed sender drains into underruns -> silence.
 *  - Source authentication (RFC 3711 / keyed RTP): NOT implemented; the
 *    engine only FILTERS by SSRC (michi_audio_session_start ssrc_filter,
 *    or first-seen SSRC registration). An SSRC change mid-stream is
 *    dropped and logged - it is NOT an authentication decision.
 *  - Multiroom sync (shared clock / absolute playhead): NOT implemented.
 *
 * ------------------------------------------------------------------
 * Session lifecycle
 * ------------------------------------------------------------------
 *   michi_audio_init():       validate the phase-11 constants and mark the
 *                             subsystem ready. Creates NO socket and NO
 *                             task: the session engine is fully idle after
 *                             boot (sessions arrive with phase 12).
 *   michi_audio_session_start(port, ssrc_filter): flushes stale audio from
 *                             the previous session (stop+start of the
 *                             michi_audio_output pipeline - its start()
 *                             flushes the ring), allocates the session
 *                             buffers (PSRAM pool) and creates the session
 *                             task. The task owns the socket; it binds and
 *                             registers itself as active. On ESP_OK the
 *                             task was created; socket failures are logged
 *                             by the task and leave the session inactive.
 *   michi_audio_session_stop(): cooperative: run flag + the task wakes on
 *                             its 100 ms socket timeout or the completed
 *                             blocking ring write, closes the socket,
 *                             frees the buffers and self-deletes; stop()
 *                             joins on a done flag with a timeout. No
 *                             external vTaskDelete, no dangling handles.
 *                             ESP_ERR_TIMEOUT: the task is still alive -
 *                             retry stop() (resources are NOT torn down
 *                             from outside).
 *   The michi_audio_output pipeline itself is started once at boot by
 *   michi_audio_boot_dac() and keeps running between sessions (continuous
 *   BCLK/LRCK keeps the PCM5122 PLL locked). Session events
 *   (MICHI_EVENT_SESSION_STARTED/CLOSED) are posted by the phase-12 API
 *   layer, not by this engine.
 *
 * ------------------------------------------------------------------
 * RTP receiver
 * ------------------------------------------------------------------
 * UDP datagram -> RTP header (v=2; CC CSRCs skipped; X extension header
 * skipped via its 16-bit length; P padding trimmed via the trailing count
 * byte). PT 10 -> PCM S16LE; PT 96 (S24LE) is declared and rejected; any
 * other PT is rejected. SSRC: registered from the first accepted packet;
 * when ssrc_filter != 0 only that SSRC is accepted; a change of SSRC
 * mid-stream drops the packet (logged; source AUTHENTICATION is a later
 * phase - see above).
 *
 * ------------------------------------------------------------------
 * Jitter buffer (packet level, ordered by 16-bit seq with wrap math)
 * ------------------------------------------------------------------
 * Capacity: MICHI_AUDIO_JITTER_MAX_MS in 10 ms packets (50 @ 500 ms).
 * Policies (metrics in parentheses):
 *  - duplicate: seq already in the queue, or behind the playhead within
 *    the window (already reproduced/passed) -> drop (duplicate).
 *  - reordered: out-of-order arrival that is still ahead of the playhead
 *    (behind the received high-water mark) -> insert sorted (reordered).
 *  - late: seq behind the playhead by more than the window -> drop (late).
 *  - loss: received seq gap > 1 -> (lost += gap - 1) at receive time and
 *    explicit silence for the missing duration at playback time.
 *  - overrun: queue full at insert -> drop-oldest + insert (overrun).
 *  - discontinuity: seq ahead of the playhead by more than the window
 *    (e.g. a sender restart without SSRC change) -> buffer flush + resync
 *    (logged; no metric slot in this phase's fixed struct).
 *  - Prefill-timeout and mid-stream SSRC-change events are logged but have
 *    NO counter slot; datagram-level rejects (malformed RTP header, PT
 *    S24LE, unknown PT, SSRC-filtered, payload geometry) are logged AND
 *    counted per class (drops_malformed, drops_pt_s24le, drops_pt_other,
 *    drops_ssrc_filtered, drops_payload_geometry).
 *
 * ------------------------------------------------------------------
 * Playback
 * ------------------------------------------------------------------
 * The session task prefills the jitter buffer (MICHI_AUDIO_PREFILL_MS)
 * before the first write; if the prefill deadline expires it starts with
 * whatever arrived (logged). Playback drains one packet per loop iteration;
 * real-time pacing comes from the michi_audio_output ring (a blocking write
 * returns when the I2S consumer drained it). Missing packets -> explicit
 * zero samples for the missing duration; underrun (empty queue) -> one
 * packet of explicit silence + a brief re-prefill that resyncs the playhead
 * to the next available packet.
 *
 * ------------------------------------------------------------------
 * Timestamps and jitter (limitation without RTCP)
 * ------------------------------------------------------------------
 * RTP timestamps size the gap silence (delta vs the last played packet,
 * sanity-bounded to the jitter window + one packet; packet-count fallback)
 * and feed a local EWMA jitter estimate (expected arrival = first arrival
 * + ts delta / sample rate; RFC 3550-style 15/16 smoothing). Without RTCP
 * the local clock is not mapped to the sender clock: drift is NOT
 * corrected in this phase - a sender that is faster/slower than real time
 * will eventually overrun (drop-oldest) or underrun (silence). Declared,
 * not implemented (see scope above).
 *
 * ------------------------------------------------------------------
 * Metrics
 * ------------------------------------------------------------------
 * Counters are updated by the session task under a short portMUX critical
 * section and copied by michi_audio_get_metrics() (phase-14 diagnostics).
 * Datagrams dropped before the stream policy (malformed RTP, rejected PT,
 * SSRC filtered) and payload-geometry rejects are logged AND counted per
 * class (drops_malformed, drops_pt_s24le, drops_pt_other,
 * drops_ssrc_filtered, drops_payload_geometry). Packets dropped by lwip
 * when the UDP receive mailbox overflows are invisible to the engine (see
 * the UDP_RECVMBOX_SIZE note at the top).
 *
 * ------------------------------------------------------------------
 * DAC integration
 * ------------------------------------------------------------------
 * michi_audio_boot_dac() starts the I2S clocks (silence) and re-runs
 * michi_dac_start(48000,16,2) so a detected-but-uninitialized DAC (phase 2:
 * no clocks) becomes initialized, then refreshes the product profile.
 * Honest: with no DAC detected the profile stays DIAGNOSTIC; if the DAC
 * still refuses to initialize, the error is logged and the profile stays
 * diagnostic (I2S clocks are left running for a later retry).
 */

/**
 * @brief Output abstraction required by the session spec; the concrete
 *        implementation is the michi_audio_output I2S pipeline.
 *
 * Declared for the phase-12 session/API layer; phase 11 uses
 * michi_audio_output_write() directly in the playback path and exposes the
 * ops so consumers (API) have one stable view of the output.
 */
typedef struct {
    /**
     * @brief Validate the output can handle the format.
     *        Phase 11: only 48000/16/2 is supported (meta 1); anything
     *        else returns ESP_ERR_NOT_SUPPORTED (declared, not implemented).
     */
    esp_err_t (*prepare)(uint32_t sample_rate, uint8_t bit_depth, uint8_t channels);
    /**
     * @brief Ensure the pipeline is running (idempotent): with the
     *        boot_dac lifecycle the pipeline is already running, so
     *        ESP_OK is returned without touching it.
     */
    esp_err_t (*start)(void);
    /** @brief Write PCM into the pipeline (michi_audio_output_write). */
    esp_err_t (*write)(const uint8_t *data, size_t len);
    /** @brief Set volume (michi_volume_set, clamped 0-100). */
    esp_err_t (*set_volume)(uint8_t volume);
    /**
     * @brief Mute: digital volume 0 via michi_volume_set() (phase 11 has
     *        no dedicated mute; get() reflects the applied value).
     *        unmute is a no-op - restore with set_volume().
     */
    esp_err_t (*mute)(bool mute);
    /** @brief Stop the pipeline (michi_audio_output_stop). */
    esp_err_t (*stop)(void);
} michi_audio_output_ops_t;

/**
 * @brief Get the output ops (implementation: michi_audio_output).
 *
 * @return Static const ops; never NULL.
 */
const michi_audio_output_ops_t *michi_audio_get_output_ops(void);

/**
 * @brief Initialize the audio engine: validate the phase-11 constants
 *        (Kconfig consistency, prefill vs jitter capacity) and mark the
 *        subsystem ready. Creates NO socket and NO task.
 *
 * Idempotent. Must be called once before session_start/boot_dac.
 *
 * @return ESP_OK; ESP_ERR_INVALID_ARG when the Kconfig constants are
 *         unusable.
 */
esp_err_t michi_audio_init(void);

/**
 * @brief Start an RTP/UDP session on `port` (phase 12 will call this from
 *        the API layer; NOT started at boot).
 *
 * Requires michi_audio_init() and a RUNNING michi_audio_output pipeline
 * (michi_audio_boot_dac() at boot). Flushes stale audio from a previous
 * session (stop + start of the pipeline), resets the metrics, allocates
 * the session buffers (jitter pool in PSRAM) and creates the session task.
 * The task binds the UDP socket and registers itself active; socket/bind
 * failures are logged by the task (session stays inactive).
 *
 * @param port        UDP port to bind (0 = ephemeral).
 * @param ssrc_filter SSRC to accept, or 0 to accept the first SSRC seen
 *                    (registered, then filtered).
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init, while a session task
 *         exists, or when the audio pipeline is not running; ESP_ERR_NO_MEM
 *         when the session buffers or the task cannot be allocated.
 */
esp_err_t michi_audio_session_start(uint16_t port, uint32_t ssrc_filter);

/**
 * @brief Cooperative session stop: run flag + join with timeout. The task
 *        closes the socket and frees the buffers itself, then self-deletes
 *        (no external vTaskDelete). Idempotent when no session is active.
 *
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init; ESP_ERR_TIMEOUT when
 *         the task did not self-delete within the join window - nothing is
 *         reset, retry stop() until ESP_OK.
 */
esp_err_t michi_audio_session_stop(void);

/**
 * @return true while a session task is running (bound socket, streaming).
 */
bool michi_audio_session_active(void);

/**
 * @brief Session metrics (phase-14 diagnostics). Counter semantics:
 *  - received:  RTP packets accepted past parse/PT/SSRC filtering.
 *  - lost:      missing packets detected from received seq gaps (gap-1).
 *  - late:      packets dropped as behind the playhead by > the window.
 *  - duplicate: packets dropped as already queued or already reproduced.
 *  - reordered: out-of-order arrivals that were still played.
 *  - underruns: jitter-buffer-empty events (one per contiguous stall; a
 *    slow-but-active sender may count one per recovered gap).
 *  - overruns:  insertions into a full queue (drop-oldest policy).
 *  - drops_*:   datagram-level rejects, counted PER CLASS (malformed RTP
 *    header, declared-but-unsupported S24LE PT, unknown PT, SSRC-filtered,
 *    payload not 16-bit-stereo aligned) - the DROP_LOG log throttle is
 *    shared, the counters are not.
 *  - jitter_us: EWMA of |arrival - expected| (no RTCP); samples clamped at
 *    1 s so sender stalls cannot pin the estimate.
 *  - buffer_ms / packets_in_buffer: live snapshot at read time.
 *  - last_seq / last_timestamp: last accepted packet.
 */
typedef struct {
    uint32_t received;
    uint32_t lost;
    uint32_t late;
    uint32_t duplicate;
    uint32_t reordered;
    uint32_t underruns;
    uint32_t overruns;
    uint32_t drops_malformed;
    uint32_t drops_pt_s24le;
    uint32_t drops_pt_other;
    uint32_t drops_ssrc_filtered;
    uint32_t drops_payload_geometry;
    uint32_t jitter_us;
    uint32_t buffer_ms;
    uint32_t packets_in_buffer;
    uint32_t last_seq;
    uint32_t last_timestamp;
} michi_audio_metrics_t;

/**
 * @brief Copy the current metrics.
 *
 * @param out Output (must not be NULL).
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init;
 *         ESP_ERR_INVALID_ARG on NULL out.
 */
esp_err_t michi_audio_get_metrics(michi_audio_metrics_t *out);

/**
 * @brief Start the I2S clocks and initialize the DAC for the validated
 *        profile (48000/16/2), then refresh the product profile.
 *
 * Called once from app_main after michi_audio_init(), only when the DAC is
 * detected-but-not-initialized (phase 2: no I2S clocks -> PLL cannot lock).
 * Initializes and starts the michi_audio_output pipeline (silence; ring
 * auto-clears), then re-runs michi_dac_start(48000,16,2) with clocks
 * running. Honest outcomes: DAC initialized -> profile refreshed and the
 * tier logged; DAC still refusing -> error returned, profile stays
 * DIAGNOSTIC, I2S clocks left running (retryable: the pipeline is
 * detected as already running and not re-created).
 *
 * @return ESP_OK (including the no-DAC-detected case, skipped with a log);
 *         pipeline init/start or michi_dac_start errors propagated.
 */
esp_err_t michi_audio_boot_dac(void);

#ifdef __cplusplus
}
#endif
