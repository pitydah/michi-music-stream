#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief RTP/UDP audio engine (MS-07: canonical receiver-lite session).
 *
 * Receives the canonical PCM S16LE RTP stream over UDP: RTP v2 without
 * CSRC/extension/padding, payload type 97, the SSRC negotiated at
 * session start (exact match - the legacy "first packet wins"
 * registration is retired), the exact IPv4 source of the HTTP request
 * that created the session, and 1920-byte payloads (10 ms at 48 kHz /
 * 16-bit / stereo). Every non-conforming datagram is rejected AND
 * counted per class (packets_rejected); the session never closes on a
 * bad packet. Sequence wrap, loss and reordering are handled with
 * 16-bit wrap math (packets_lost) without closing the session.
 *
 * The datagram validation lives in rtp_guard.c - the SAME source the
 * host-side tests compile (tests/host) - so packet acceptance is proven
 * by shared code, not by a reimplementation.
 *
 * Resilience comes from the bounded per-session jitter buffer
 * (MICHI_AUDIO_JITTER_MAX_MS), the SPSC ring of michi_audio_output and
 * lwip's UDP receive mailbox (CONFIG_LWIP_UDP_RECVMBOX_SIZE, raised in
 * sdkconfig.defaults so bursts land in the jitter buffer instead of the
 * socket). The unsynchronized 4 MB global buffer of the legacy firmware
 * is NOT used.
 *
 * ------------------------------------------------------------------
 * Session lifecycle
 * ------------------------------------------------------------------
 *   michi_audio_init():       validate the canonical constants and mark
 *                             the subsystem ready. Creates NO socket and
 *                             NO task: the session engine is fully idle
 *                             after boot.
 *   michi_audio_session_start(port, ssrc, source_ip): ALL-OR-NOTHING.
 *                             Flushes stale audio from the previous
 *                             session, allocates the session buffers,
 *                             creates and BINDS the UDP socket
 *                             synchronously (port 0 = the receiver picks
 *                             a free port in 49152..65535) and creates
 *                             the session task. Any failure (socket,
 *                             bind, buffers, task, pipeline) tears down
 *                             everything already reserved and returns
 *                             the error: the caller (michi_session)
 *                             rolls back to idle - a phantom session is
 *                             impossible by construction.
 *   michi_audio_session_stop(): cooperative: run flag + the task wakes
 *                             on its 100 ms socket timeout or the
 *                             completed blocking ring write, closes the
 *                             socket, frees the buffers and self-deletes;
 *                             stop() joins on a done flag with a
 *                             timeout. No external vTaskDelete, no
 *                             dangling handles. ESP_ERR_TIMEOUT: the
 *                             task is still alive - retry stop().
 *   michi_audio_session_set_paused(bool): pause keeps the socket and
 *                             the task ALIVE (the canonical Paused state
 *                             is not a teardown): valid packets keep
 *                             being received and counted but are
 *                             discarded (silence); resume flushes the
 *                             jitter buffer and resyncs to the next
 *                             expected sequence.
 *   The michi_audio_output pipeline itself is started once at boot by
 *   michi_audio_boot_dac() and keeps running between sessions
 *   (continuous BCLK/LRCK keeps the PCM5122 PLL locked). Session events
 *   are posted by the session layer, not by this engine.
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
 *  - Prefill-timeout events are logged but have NO counter slot;
 *    datagram-level rejects (malformed RTP header, PT mismatch, SSRC
 *    mismatch, source-IP mismatch, payload size mismatch) are logged AND
 *    counted per class (drops_malformed, drops_pt_other,
 *    drops_ssrc_filtered, drops_source_ip, drops_payload_geometry).
 *
 * ------------------------------------------------------------------
 * Playback
 * ------------------------------------------------------------------
 * The session task prefills the jitter buffer (MICHI_AUDIO_PREFILL_MS)
 * before the first write; if the prefill deadline expires it starts with
 * whatever arrived (logged). Playback drains one packet per loop
 * iteration; real-time pacing comes from the michi_audio_output ring (a
 * blocking write returns when the I2S consumer drained it). Missing
 * packets -> explicit zero samples for the missing duration; underrun
 * (empty queue) -> one packet of explicit silence + a brief re-prefill
 * that resyncs the playhead to the next available packet.
 *
 * ------------------------------------------------------------------
 * Timestamps and jitter (limitation without RTCP)
 * ------------------------------------------------------------------
 * RTP timestamps size the gap silence (delta vs the last played packet,
 * sanity-bounded to the jitter window + one packet; packet-count
 * fallback) and feed a local EWMA jitter estimate (expected arrival =
 * first arrival + ts delta / sample rate; RFC 3550-style 15/16
 * smoothing). Without RTCP the local clock is not mapped to the sender
 * clock: drift is NOT corrected in this phase - a sender that is faster
 * or slower than real time will eventually overrun (drop-oldest) or
 * underrun (silence). Declared, not implemented.
 *
 * ------------------------------------------------------------------
 * Metrics
 * ------------------------------------------------------------------
 * Counters are updated by the session task under a short portMUX
 * critical section and copied by michi_audio_get_metrics(). Datagram
 * rejects are counted per class; the session layer exposes their sum as
 * packets_rejected. Packets dropped by lwip when the UDP receive
 * mailbox overflows are invisible to the engine (see the
 * UDP_RECVMBOX_SIZE note at the top).
 *
 * ------------------------------------------------------------------
 * DAC integration
 * ------------------------------------------------------------------
 * michi_audio_boot_dac() starts the I2S clocks (silence) and re-runs
 * michi_dac_start(48000,16,2) so a detected-but-uninitialized DAC
 * becomes initialized, then refreshes the product profile. Honest: with
 * no DAC detected the profile stays DIAGNOSTIC; if the DAC still refuses
 * to initialize, the error is logged and the profile stays diagnostic
 * (I2S clocks are left running for a later retry).
 */

/**
 * @brief Output abstraction required by the session spec; the concrete
 *        implementation is the michi_audio_output I2S pipeline.
 *
 * Declared for the session/API layer; the engine uses
 * michi_audio_output_write() directly in the playback path and exposes
 * the ops so consumers (API) have one stable view of the output.
 */
typedef struct {
    /**
     * @brief Validate the output can handle the format.
     *        Only 48000/16/2 is supported (the canonical profile);
     *        anything else returns ESP_ERR_NOT_SUPPORTED.
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
     * @brief Mute: digital volume 0 via michi_volume_set(). unmute is
     *        a no-op - restore with set_volume().
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
 * @brief Initialize the audio engine: validate the canonical constants
 *        (PT 97, receive buffer >= header + 1920-byte payload, prefill
 *        vs jitter capacity) and mark the subsystem ready. Creates NO
 *        socket and NO task.
 *
 * Idempotent. Must be called once before session_start/boot_dac.
 *
 * @return ESP_OK; ESP_ERR_INVALID_ARG when a canonical constant is
 *         unusable.
 */
esp_err_t michi_audio_init(void);

/**
 * @brief Start the canonical RTP/UDP session. ALL-OR-NOTHING.
 *
 * Requires michi_audio_init() and a RUNNING michi_audio_output pipeline
 * (michi_audio_boot_dac() at boot). Flushes stale audio from a previous
 * session (stop + start of the pipeline), resets the metrics, allocates
 * the session buffers (jitter pool in PSRAM), creates the UDP socket and
 * BINDS it synchronously, then creates the session task. On ANY failure
 * (socket, bind, buffers, task, pipeline down) every resource already
 * reserved is released and the error is returned - the caller rolls
 * back to idle, never a phantom session.
 *
 * The SSRC is the EXACT negotiated value (1..4294967295): packets with
 * any other SSRC are rejected and counted - there is no first-packet-
 * wins registration. The source IP filter is set to `source_ip` (the
 * HTTP request peer, dotted IPv4): datagrams from any other address are
 * rejected and counted.
 *
 * @param port      UDP port to bind, or 0 = the receiver picks a free
 *                  port in 49152..65535 (the canonical range). The bound
 *                  port is readable with michi_audio_session_get_port().
 * @param ssrc      Negotiated SSRC (1..4294967295; 0 is invalid).
 * @param source_ip Dotted IPv4 of the HTTP request peer (the only
 *                  accepted RTP source).
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init, while a session
 *         task exists, or when the audio pipeline is not running;
 *         ESP_ERR_INVALID_ARG for an unusable SSRC/source_ip or when
 *         port is outside 49152..65535 (and not 0);
 *         ESP_ERR_NO_MEM when the session buffers or the task cannot
 *         be allocated; ESP_FAIL on socket/bind failure.
 */
esp_err_t michi_audio_session_start(uint16_t port, uint32_t ssrc,
                                    const char *source_ip);

/**
 * @brief Cooperative session stop: run flag + join with timeout. The
 *        task closes the socket and frees the buffers itself, then
 *        self-deletes (no external vTaskDelete). Idempotent when no
 *        session is active.
 *
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init; ESP_ERR_TIMEOUT
 *         when the task did not self-delete within the join window -
 *         nothing is reset, retry stop() until ESP_OK.
 */
esp_err_t michi_audio_session_stop(void);

/**
 * @return true while a session task is running (bound socket,
 *         streaming or paused - pause keeps the task alive).
 */
bool michi_audio_session_active(void);

/**
 * @brief Pause/resume the session WITHOUT tearing it down.
 *
 * Paused: the task keeps receiving and counting valid packets but
 * discards them (silence; the jitter buffer is NOT filled). Resume:
 * the jitter buffer is flushed and the playhead resyncs to the next
 * expected sequence - the session continues without rebinding.
 *
 * @param paused true to pause, false to resume.
 */
void michi_audio_session_set_paused(bool paused);

/**
 * @brief Get the UDP port the session is bound to.
 *
 * @param out_port Receives the port (49152..65535).
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init; ESP_ERR_INVALID_ARG
 *         on NULL out_port; ESP_ERR_NOT_FOUND when no session is active.
 */
esp_err_t michi_audio_session_get_port(uint16_t *out_port);

/**
 * @brief Get the SSRC of the current session (the negotiated value,
 *        set at session start - no first-packet registration).
 *
 * @param out_ssrc Receives the SSRC (network-order value).
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init; ESP_ERR_INVALID_ARG
 *         on NULL out_ssrc; ESP_ERR_NOT_FOUND when no session is active.
 */
esp_err_t michi_audio_session_get_ssrc(uint32_t *out_ssrc);

/**
 * @brief Get the session's expected RTP source IP as a dotted string
 *        (the HTTP request peer, set at session start).
 *
 * @param out     Buffer for the dotted IPv4 string.
 * @param out_len Size of out (16 bytes hold the longest IPv4 string).
 * @return ESP_OK; ESP_ERR_INVALID_STATE before init; ESP_ERR_INVALID_ARG
 *         on NULL out / zero length; ESP_ERR_NOT_FOUND when no session
 *         is active; ESP_ERR_INVALID_SIZE when the buffer cannot hold
 *         the string (never truncated).
 */
esp_err_t michi_audio_session_get_peer(char *out, size_t out_len);

/**
 * @brief Session metrics. Counter semantics:
 *  - received:  RTP packets accepted past the guard (PT/SSRC/source/size).
 *  - lost:      missing packets detected from received seq gaps (gap-1).
 *  - late:      packets dropped as behind the playhead by > the window.
 *  - duplicate: packets dropped as already queued or already reproduced.
 *  - reordered: out-of-order arrivals that were still played.
 *  - underruns: jitter-buffer-empty events (one per contiguous stall).
 *  - overruns:  insertions into a full queue (drop-oldest policy).
 *  - drops_*:   datagram-level rejects, counted PER CLASS (malformed
 *    RTP header, non-97 PT, non-negotiated SSRC, wrong source IP,
 *    payload not 1920 bytes) - the DROP_LOG log throttle is shared,
 *    the counters are not. packets_rejected (the API counter) is their
 *    sum, computed by the session layer.
 *  - jitter_us: EWMA of |arrival - expected| (no RTCP); samples clamped
 *    at 1 s so sender stalls cannot pin the estimate.
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
    uint32_t drops_pt_other;
    uint32_t drops_ssrc_filtered;
    uint32_t drops_source_ip;
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
 * @brief Start the I2S clocks and initialize the DAC for the canonical
 *        profile (48000/16/2), then refresh the product profile.
 *
 * Called once from app_main after michi_audio_init(), only when the DAC
 * is detected-but-not-initialized. Initializes and starts the
 * michi_audio_output pipeline (silence; ring auto-clears), then re-runs
 * michi_dac_start(48000,16,2) with clocks running. Honest outcomes: DAC
 * initialized -> profile refreshed and the tier logged; DAC still
 * refusing -> error returned, profile stays DIAGNOSTIC, I2S clocks left
 * running (retryable).
 *
 * @return ESP_OK (including the no-DAC-detected case, skipped with a
 *         log); pipeline init/start or michi_dac_start errors
 *         propagated.
 */
esp_err_t michi_audio_boot_dac(void);

#ifdef __cplusplus
}
#endif
