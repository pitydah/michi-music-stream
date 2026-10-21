/*
 * RTP/UDP audio engine (MS-07: canonical receiver-lite session).
 *
 * Design (see include/michi_audio.h for the full contract):
 *  - The session task owns the UDP socket and the jitter buffer; the
 *    socket is created and BOUND SYNCHRONOUSLY by
 *    michi_audio_session_start() (port 0 = the receiver picks a free
 *    port in 49152..65535) so a bind/socket failure is returned to the
 *    caller BEFORE any session state exists - the caller rolls back to
 *    idle and a phantom session is impossible. The task self-deletes
 *    after cooperative shutdown (no external vTaskDelete).
 *  - Every datagram is validated by the SHARED guard (rtp_guard.c):
 *    RTP v2 without CSRC/extension/padding, PT 97, the EXACT negotiated
 *    SSRC (first-packet-wins is retired), the EXACT HTTP request peer
 *    IPv4 and a 1920-byte payload. Rejects are counted per class
 *    (packets_rejected = their sum) and never close the session.
 *  - Packet-level jitter buffer: ordered by 16-bit seq with wrap-aware
 *    int16 diff math; policies: duplicate, reordered, late, loss,
 *    overrun (preserve imminent PCM), discontinuity (flush + resync). Sequence
 *    wrap, loss and reordering never close the session.
 *  - Pause keeps the task ALIVE: valid packets are counted (received +
 *    loss) and discarded; resume flushes the buffer and resyncs to the
 *    next expected sequence.
 *  - Playback drains one packet per loop iteration; real-time pacing
 *    comes from the michi_audio_output ring (blocking write = ring flow
 *    control). Gaps and underruns write EXPLICIT zero samples.
 *  - Metrics are updated by the session task under a short portMUX
 *    critical section (single writer, arbitrary readers).
 *  - No 4 MB global buffer: bounded per-session PSRAM pool + lwip UDP
 *    receive mailbox.
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "michi_audio.h"
#include "michi_audio_output.h"
#include "michi_board.h"
#include "michi_dac.h"
#include "michi_product_profile.h"
#include "michi_state.h"
#include "michi_volume.h"
#include "michi_rtp_clock.h"
#include "michi_rtp_jitter.h"
#include "michi_jb.h"
#include "michi_audio_recv_policy.h"
#include "michi_audio_pause_policy.h"
#include "rtp_guard.h"

#define TAG "michi_audio"

/* Canonical profile (contract section 2.5) - the validated baseline. */
#define MICHI_AUDIO_SAMPLE_RATE 48000
#define MICHI_AUDIO_BIT_DEPTH   16
#define MICHI_AUDIO_CHANNELS    2
#define MICHI_AUDIO_BYTES_PER_SAMPLE (MICHI_AUDIO_BIT_DEPTH / 8)

/* Canonical RTP constants (shared with rtp_guard.c - see rtp_guard.h). */
#define MICHI_AUDIO_RTP_PT_S16LE MICHI_RTP_GUARD_PT_S16LE
#define MICHI_AUDIO_RTP_PAYLOAD_BYTES MICHI_RTP_GUARD_PAYLOAD_BYTES
#define MICHI_AUDIO_PACKET_MS_ASSUMED 10 /* capacity unit: 10 ms packets */
/* 10 ms at 48 kHz = 480 frames = 960 samples per packet. */
#define MICHI_AUDIO_SAMPLES_PER_PACKET \
    (MICHI_AUDIO_SAMPLE_RATE / 1000 * MICHI_AUDIO_PACKET_MS_ASSUMED)

/* The receiver picks the UDP port in 49152..65535 (contract 2.5). */
#define MICHI_AUDIO_STREAM_PORT_MIN 49152
#define MICHI_AUDIO_STREAM_PORT_MAX 65535

#define MICHI_AUDIO_SESSION_SOCKET_TIMEOUT_MS 100 /* wake cadence for stop() */
#define MICHI_AUDIO_PREFILL_DEADLINE_MS 1000      /* start-with-what-we-have */
#define MICHI_AUDIO_REPREFILL_MS        250       /* brief re-prefill after underrun */
#define MICHI_AUDIO_JOIN_TIMEOUT_MS     2000      /* session task join window */
#define MICHI_AUDIO_SESSION_TASK_PRIO   7         /* below the I2S consumer (8) */
#define MICHI_AUDIO_JITTER_EWMA_SHIFT   4         /* /16 smoothing, RFC 3550 style */

/* Jitter buffer capacity in packets, in 10 ms units (spec convention). */
#define MICHI_AUDIO_MAX_PACKETS (CONFIG_MICHI_AUDIO_JITTER_MAX_MS / \
                                 MICHI_AUDIO_PACKET_MS_ASSUMED)

#define MICHI_AUDIO_RX_BUF_BYTES CONFIG_MICHI_AUDIO_RX_BUF_BYTES
/* A UDP receive datagram includes RTP headers, but jitter slots contain ONLY
 * the validated PCM payload. Do not multiply the UDP datagram capacity by
 * the number of jitter slots: 50 * 8192 wastes >300 KiB of PSRAM. */
#define MICHI_AUDIO_JB_SLOT_BYTES MICHI_AUDIO_RTP_PAYLOAD_BYTES

typedef michi_jb_entry_t jb_entry_t;
typedef michi_jb_t jitter_buffer_t;

/* Per-session state, owned by the session task (allocated in
 * michi_audio_session_start, freed by the task on exit). */
typedef struct {
    int      sock;      /* bound synchronously at start */
    uint16_t port;
    uint32_t ssrc;      /* EXACT negotiated SSRC (no first-packet-wins) */
    michi_rtp_guard_session_t guard; /* negotiated validation constants */

    /* Stream state */
    bool     stream_seeded;  /* first accepted packet seeded the playhead */
    uint16_t playhead;       /* next seq to play */
    uint16_t last_seq;       /* received high-water mark */
    uint32_t last_played_ts; /* RTP ts of the last played packet; 0 = none */
    uint32_t samples_per_packet; /* canonical: 480 */
    uint32_t base_ts;        /* first packet ts (jitter reference, raw) */
    int64_t  base_time_us;   /* first packet arrival (jitter reference) */
    uint32_t jitter_us;      /* EWMA estimate */
    bool     in_underrun;    /* one underrun counted per contiguous stall */
    uint32_t drop_log_count; /* rogue-source log throttle */
    uint8_t  recv_error_streak; /* unexpected consecutive UDP receive failures */
    bool     fatal_recv_error;  /* distinguish terminal socket fault from timeout */

    /* Extended 64-bit RTP clock (P0-02: wrap fix, see michi_rtp_clock.h).
     * Tracks the monotonic 64-bit form of the RTP timestamp to prevent
     * uint32_t wrap at ~24.85h causing a corrupted jitter reading. */
    michi_rtp_clock_t rtp_clock;
    michi_rtp_jitter_t rtp_jitter;
    int32_t  clock_offset_us;

    uint16_t buffer_ms;      /* negotiated jitter target (50..500 ms) */
    jitter_buffer_t jb;
    uint8_t *recv_buf;       /* datagram buffer (heap) */
    uint8_t *zeros;          /* one packet of silence (heap) */
} session_t;

/* H13: task-to-control communication requires actual C11 synchronization,
 * not volatile. Default sequentially-consistent atomic operations are used
 * for the cooperative run/done/paused flags; multi-field snapshots remain
 * under s_lock. Task handle publication is atomic so that a task that exits
 * immediately after creation cannot race an unsynchronized handle store. */
static _Atomic bool s_initialized = false;
static _Atomic bool s_session_run = false;
static _Atomic bool s_session_done = false;
static _Atomic bool s_session_active = false;
static _Atomic(TaskHandle_t) s_session_task = NULL;
static _Atomic bool s_paused = false;

/* H13: serialize start/stop/pause transitions without a spinlock around
 * network, I2S, task creation or waits. A concurrent request fails closed.
 * Session worker itself never needs this lease. */
static portMUX_TYPE s_session_lifecycle_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_session_lifecycle_busy = false;
static bool session_lifecycle_try_enter(void)
{
    portENTER_CRITICAL(&s_session_lifecycle_lock);
    bool acquired = !s_session_lifecycle_busy;
    if (acquired) s_session_lifecycle_busy = true;
    portEXIT_CRITICAL(&s_session_lifecycle_lock);
    return acquired;
}
static void session_lifecycle_leave(void)
{
    portENTER_CRITICAL(&s_session_lifecycle_lock);
    s_session_lifecycle_busy = false;
    portEXIT_CRITICAL(&s_session_lifecycle_lock);
}

/* Negotiated session constants (set at session start, read by the API
 * layer / diagnostics while the session is active). Kept OUT of
 * session_t on purpose: session_t is owned and freed by the session
 * task, these snapshots must survive it. */
static uint32_t s_session_ssrc;      /* guarded by s_lock */
static struct in_addr s_session_peer; /* guarded by s_lock */
static uint16_t s_session_port;      /* guarded by s_lock */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static michi_audio_metrics_t s_metrics;

/* ------------------------------------------------------------------
 * Metrics (single writer: the session task; readers lock)
 * ------------------------------------------------------------------ */

static void m_add(uint32_t *field, uint32_t v)
{
    portENTER_CRITICAL(&s_lock);
    *field += v;
    portEXIT_CRITICAL(&s_lock);
}

static void m_sub(uint32_t *field, uint32_t v)
{
    portENTER_CRITICAL(&s_lock);
    if (*field >= v) {
        *field -= v;
    } else {
        *field = 0;
    }
    portEXIT_CRITICAL(&s_lock);
}

static void m_set(uint32_t *field, uint32_t v)
{
    portENTER_CRITICAL(&s_lock);
    *field = v;
    portEXIT_CRITICAL(&s_lock);
}

static void m_set_i32(int32_t *field, int32_t v)
{
    portENTER_CRITICAL(&s_lock);
    *field = v;
    portEXIT_CRITICAL(&s_lock);
}

static void metrics_live(const session_t *s)
{
    uint32_t ms = 0;
    if (s->samples_per_packet != 0) {
        ms = (uint32_t)((uint64_t)s->jb.count * s->samples_per_packet * 1000 /
                        MICHI_AUDIO_SAMPLE_RATE);
    }
    portENTER_CRITICAL(&s_lock);
    s_metrics.packets_in_buffer = s->jb.count;
    s_metrics.buffer_ms = ms;
    portEXIT_CRITICAL(&s_lock);
}

/* Log the first drop of a class, then every 100th (rogue-source spam guard). */
#define DROP_LOG(s, ...)                                     \
    do {                                                     \
        uint32_t n_ = (s)->drop_log_count++;                 \
        if (n_ == 0 || (n_ % 100) == 0) {                    \
            ESP_LOGW(TAG, __VA_ARGS__);                      \
        }                                                    \
    } while (0)

/* ------------------------------------------------------------------
 * Jitter buffer (packet level, ordered by 16-bit seq)
 * ------------------------------------------------------------------ */

static inline jb_entry_t *jb_slot_by_index(jitter_buffer_t *jb, uint32_t idx)
{
    return michi_jb_entry_by_index(jb, idx);
}

static inline uint32_t jb_index(const jitter_buffer_t *jb, const jb_entry_t *e)
{
    return michi_jb_entry_index(jb, e);
}

static inline jb_entry_t *jb_find_seq(jitter_buffer_t *jb, uint16_t seq)
{
    return michi_jb_find(jb, seq);
}

static inline jb_entry_t *jb_oldest(jitter_buffer_t *jb, uint16_t playhead)
{
    return michi_jb_oldest(jb, playhead);
}

static inline void jb_flush(jitter_buffer_t *jb)
{
    michi_jb_flush(jb);
}

/* Insert a copy of the payload. Returns ESP_ERR_INVALID_STATE when the seq
 * is already queued (caller already classified it as duplicate); applies
 * the playout-first overrun policy when the buffer is full: reclaim stale
 * slots or evict the farthest future PCM only for a nearer packet. Rejected
 * packets are counted as overruns and are never inserted. */
static esp_err_t jb_insert(session_t *s, uint16_t playhead,
                           const michi_rtp_guard_packet_t *pkt)
{
    uint16_t evicted_seq = 0;
    michi_jb_insert_result_t res = michi_jb_insert_packet(
        &s->jb, playhead, pkt->seq, pkt->timestamp,
        pkt->payload, pkt->payload_len, &evicted_seq);

    if (res == MICHI_JB_INSERT_DUPLICATE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (res == MICHI_JB_INSERT_OVERRUN_EVICTED) {
        ESP_LOGW(TAG, "jitter buffer full: reclaimed stale/farthest seq=%u",
                 (unsigned)evicted_seq);
        m_add(&s_metrics.overruns, 1);
        return ESP_OK;
    }
    if (res == MICHI_JB_INSERT_OVERRUN_DROPPED) {
        DROP_LOG(s, "jitter buffer rejected seq=%u (late or less useful "
                    "than queued PCM)", (unsigned)pkt->seq);
        m_add(&s_metrics.overruns, 1);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------
 * Stream policy (see header for the full taxonomy)
 * ------------------------------------------------------------------ */

static bool stream_policy(session_t *s, const michi_rtp_guard_packet_t *pkt)
{
    const int16_t max_pkts = (int16_t)MICHI_AUDIO_MAX_PACKETS;

    if (s_paused) {
        /* Paused: valid packets are counted (received + loss
         * accounting) but never queued - silence, not a buffer leak.
         * Sequence gaps observed during pause represent network missing packets,
         * not playout underruns or buffer starvation. */
        const int16_t diff_l = (int16_t)(pkt->seq - s->last_seq);
        const uint32_t lost = michi_rtp_guard_lost_delta(s->last_seq,
                                                         pkt->seq);
        if (lost != 0) {
            m_add(&s_metrics.lost, lost);
        }
        if (diff_l > 0) {
            s->last_seq = pkt->seq;
        }
        return true; /* consumed (counted and discarded) */
    }

    if (!s->stream_seeded) {
        /* First accepted packet: seed the playhead and the jitter reference.
         * Initialise the 64-bit extended clock at the same time (P0-02). */
        s->stream_seeded = true;
        s->playhead = pkt->seq;
        s->last_seq = pkt->seq;
        s->base_ts  = pkt->timestamp;
        s->base_time_us = esp_timer_get_time();
        m_set(&s_metrics.provisionally_missing, 0);
        /* michi_rtp_clock_feed seeds .extended and .base on first call. */
        michi_rtp_clock_reset(&s->rtp_clock);
        michi_rtp_clock_feed(&s->rtp_clock, pkt->timestamp);
        michi_rtp_jitter_reset(&s->rtp_jitter);
        michi_rtp_jitter_feed(&s->rtp_jitter, s->base_time_us, pkt->timestamp,
                              MICHI_AUDIO_SAMPLE_RATE);
        (void)jb_insert(s, s->playhead, pkt);
        return true;
    }

    const int16_t diff_p = (int16_t)(pkt->seq - s->playhead);

    if (diff_p > max_pkts) {
        /* Ahead of the playhead by more than the window: stream
         * discontinuity (sender restart). Flush + resync; the buffered
         * packets are obsolete.  Reset the 64-bit extended clock and jitter filter
         * so the new epoch does not carry the old base (P0-02). */
        ESP_LOGW(TAG, "seq %u ahead of playhead %u by more than the window: "
                      "buffer flush + resync",
                 (unsigned)pkt->seq, (unsigned)s->playhead);
        jb_flush(&s->jb);
        s->playhead = pkt->seq;
        s->last_seq = pkt->seq;
        s->last_played_ts = 0;
        s->base_ts  = pkt->timestamp;
        s->base_time_us = esp_timer_get_time();
        m_set(&s_metrics.provisionally_missing, 0);
        /* Reset extended clock and jitter filter for the new sender epoch. */
        michi_rtp_clock_reset(&s->rtp_clock);
        michi_rtp_clock_feed(&s->rtp_clock, pkt->timestamp);
        michi_rtp_jitter_reset(&s->rtp_jitter);
        michi_rtp_jitter_feed(&s->rtp_jitter, s->base_time_us, pkt->timestamp,
                              MICHI_AUDIO_SAMPLE_RATE);
        (void)jb_insert(s, s->playhead, pkt);
        return true;
    }
    if (diff_p < 0) {
        /* Behind the playhead: arrived after its playout deadline -> late */
        m_add(&s_metrics.late, 1);
        return false;
    }

    const int16_t diff_l = (int16_t)(pkt->seq - s->last_seq);
    const bool reordered = (diff_l <= 0); /* out of order, still playable */

    const esp_err_t err = jb_insert(s, s->playhead, pkt);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_STATE) {
            m_add(&s_metrics.duplicate, 1);
        }
        /* Do not advance received high-water or provisional loss on a
         * packet refused by the playout buffer. */
        return false;
    }
    const uint32_t missing = michi_rtp_guard_lost_delta(s->last_seq, pkt->seq);
    if (missing != 0) {
        m_add(&s_metrics.provisionally_missing, missing);
    }
    if (reordered) {
        m_add(&s_metrics.reordered, 1);
        m_sub(&s_metrics.provisionally_missing, 1);
    }
    /* The high-water mark advances only on accepted in-order arrivals. */
    if (diff_l > 0) {
        s->last_seq = pkt->seq;
    }
    return true;
}

/* ------------------------------------------------------------------
 * Session receive path
 * ------------------------------------------------------------------ */

static void session_recv(session_t *s)
{
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    const int n = recvfrom(s->sock, s->recv_buf, MICHI_AUDIO_RX_BUF_BYTES, 0,
                           (struct sockaddr *)&from, &from_len);
    if (n < 0) {
        const int socket_error = errno;
        if (michi_audio_recv_error_is_fatal(&s->recv_error_streak, socket_error)) {
            /* Unexpected persistent socket failures must terminate the RTP
             * session. Ordinary recv timeouts/EINTR never end a session. */
            ESP_LOGE(TAG, "RTP socket: %u consecutive receive errors; "
                          "last errno=%d; closing session",
                     (unsigned)s->recv_error_streak, socket_error);
            s->fatal_recv_error = true;
            s_session_run = false;
        }
        return;
    }
    michi_audio_recv_error_reset(&s->recv_error_streak);
    if (n == 0) {
        /* A zero-byte UDP datagram is not PCM; do not confuse it with a
         * receive timeout. Drop it as a malformed datagram. */
        m_add(&s_metrics.drops_malformed, 1);
        return;
    }

    michi_rtp_guard_packet_t pkt;
    if (!michi_rtp_guard_parse(s->recv_buf, (size_t)n, &pkt)) {
        DROP_LOG(s, "malformed RTP datagram (%d bytes)", n);
        m_add(&s_metrics.drops_malformed, 1);
        return;
    }
    const michi_rtp_guard_verdict_t verdict = michi_rtp_guard_classify(
        &pkt, &s->guard, from.sin_addr.s_addr);
    switch (verdict) {
    case MICHI_RTP_GUARD_OK:
        break;
    case MICHI_RTP_GUARD_PT_MISMATCH:
        DROP_LOG(s, "RTP PT %u rejected (canonical PT is %u)",
                 (unsigned)pkt.pt, (unsigned)MICHI_AUDIO_RTP_PT_S16LE);
        m_add(&s_metrics.drops_pt_other, 1);
        return;
    case MICHI_RTP_GUARD_SSRC_MISMATCH:
        DROP_LOG(s, "RTP SSRC 0x%08" PRIx32 " rejected (session accepts "
                    "0x%08" PRIx32 ")",
                 pkt.ssrc, s->ssrc);
        m_add(&s_metrics.drops_ssrc_filtered, 1);
        return;
    case MICHI_RTP_GUARD_SOURCE_MISMATCH:
        DROP_LOG(s, "RTP datagram rejected: source IP is not the HTTP "
                    "request peer");
        m_add(&s_metrics.drops_source_ip, 1);
        return;
    case MICHI_RTP_GUARD_SIZE_MISMATCH:
        DROP_LOG(s, "payload %u bytes rejected (canonical payload is %u "
                    "bytes)",
                 (unsigned)pkt.payload_len,
                 (unsigned)MICHI_AUDIO_RTP_PAYLOAD_BYTES);
        m_add(&s_metrics.drops_payload_geometry, 1);
        return;
    case MICHI_RTP_GUARD_MALFORMED:
    default:
        /* Unreachable: the parse above succeeded. */
        m_add(&s_metrics.drops_malformed, 1);
        return;
    }

    m_add(&s_metrics.received, 1);

    if (!stream_policy(s, &pkt)) {
        return; /* dropped by the policy (metrics already updated) */
    }

    m_set(&s_metrics.last_seq, pkt.seq);
    m_set(&s_metrics.last_timestamp, pkt.timestamp);
    metrics_live(s);

    const int64_t now_us = esp_timer_get_time();

    /* RFC 3550 transit-difference interarrival jitter filter */
    s->jitter_us = michi_rtp_jitter_feed(&s->rtp_jitter, now_us, pkt.timestamp,
                                         MICHI_AUDIO_SAMPLE_RATE);
    m_set(&s_metrics.jitter_us, s->jitter_us);
    m_set(&s_metrics.rtp_interarrival_jitter_us, s->jitter_us);

    /* Clock offset tracking (cumulative sender vs receiver drift) */
    michi_rtp_clock_feed(&s->rtp_clock, pkt.timestamp);
    const uint64_t ts_delta_64 = michi_rtp_clock_delta(&s->rtp_clock);
    const int32_t offset_us = michi_rtp_clock_offset_us(now_us, s->base_time_us,
                                                         ts_delta_64,
                                                         MICHI_AUDIO_SAMPLE_RATE);
    s->clock_offset_us = offset_us;
    m_set_i32(&s_metrics.clock_offset_us, offset_us);
}

/* ------------------------------------------------------------------
 * Playback path
 * ------------------------------------------------------------------ */

/* Prefill target in packets (ceil of prefill_ms / packet duration). */
static uint32_t prefill_target(const session_t *s)
{
    uint16_t ms = (s != NULL && s->buffer_ms > 0) ? s->buffer_ms
                                                   : CONFIG_MICHI_AUDIO_PREFILL_MS;
    return michi_audio_calculate_prefill_target_ext(ms, MICHI_AUDIO_MAX_PACKETS);
}

/* Receive + insert until the buffer holds prefill_target(s) packets, the
 * deadline passes, the session is paused or the session is stopped. */
static void session_fill(session_t *s, uint32_t deadline_ms)
{
    const uint32_t start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    for (;;) {
        if (!s_session_run || s_paused || s->jb.count >= prefill_target(s)) {
            return;
        }
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - start_ms >= deadline_ms) {
            return;
        }
        session_recv(s);
    }
}

/* Write `samples` of explicit silence, one packet per blocking write (the
 * ring paces them in real time). Returns false when the pipeline stopped. */
static bool write_silence(session_t *s, uint32_t samples)
{
    while (samples > 0 && s_session_run) {
        uint32_t chunk = samples < s->samples_per_packet ? samples
                                                         : s->samples_per_packet;
        if (chunk == 0) {
            break;
        }
        const size_t bytes = (size_t)chunk * MICHI_AUDIO_CHANNELS *
                             MICHI_AUDIO_BYTES_PER_SAMPLE;
        if (michi_audio_output_write(s->zeros, bytes) != ESP_OK) {
            return false;
        }
        samples -= chunk;
    }
    return true;
}

/* Silence to insert before playing `next`, sized from RTP timestamps when
 * contiguous (delta sane), else from the packet count. */
static uint32_t gap_samples(const session_t *s, const jb_entry_t *next)
{
    const uint32_t missing = (uint32_t)(uint16_t)(next->seq - s->playhead);
    if (s->last_played_ts != 0) {
        const uint32_t delta = next->timestamp - s->last_played_ts;
        const uint32_t max_delta = (MICHI_AUDIO_MAX_PACKETS + 1) *
                                   s->samples_per_packet;
        if (delta > s->samples_per_packet && delta <= max_delta) {
            return delta - s->samples_per_packet; /* ts-based (contiguous) */
        }
    }
    return missing * s->samples_per_packet; /* count-based fallback */
}

/* Drain one packet (gap silence first when the playhead is behind). Returns
 * false when the pipeline rejected the write (session must end). */
static bool session_drain(session_t *s)
{
    /* A packet that was once in-range can become obsolete after playout
     * advances. michi_jb_oldest() deliberately returns the closest packet
     * BEHIND the playhead if all slots are stale (used for buffer eviction).
     * Never interpret that negative sequence distance as a uint16_t gap:
     * it would turn a -1 packet into 65535 packets of silence. */
    const uint32_t stale = michi_jb_discard_stale(&s->jb, s->playhead);
    if (stale != 0) {
        m_add(&s_metrics.late, stale);
        metrics_live(s);
    }
    jb_entry_t *pkt = jb_oldest(&s->jb, s->playhead);
    if (pkt == NULL) {
        return true; /* empty: the underrun path handles it */
    }

    if (pkt->seq != s->playhead) {
        const uint32_t skipped = (uint32_t)(uint16_t)(pkt->seq - s->playhead);
        m_add(&s_metrics.lost, skipped);
        m_sub(&s_metrics.provisionally_missing, skipped);
        const uint32_t gap = gap_samples(s, pkt);
        ESP_LOGD(TAG, "gap: playhead=%u next=%u silence=%" PRIu32 " samples",
                 (unsigned)s->playhead, (unsigned)pkt->seq, gap);
        if (!write_silence(s, gap)) {
            return false;
        }
        s->playhead = pkt->seq;
        pkt = jb_oldest(&s->jb, s->playhead);
        if (pkt == NULL || pkt->seq != s->playhead) {
            return true; /* defensive: nothing more to drain */
        }
    }

    const uint32_t idx = jb_index(&s->jb, pkt);
    if (michi_audio_output_write(s->jb.pool + (size_t)idx * MICHI_AUDIO_JB_SLOT_BYTES,
                                 pkt->len) != ESP_OK) {
        return false;
    }
    /* H19: only accepted RTP PCM successfully admitted to the I2S ring
     * counts as submitted audio. Gap/underrun silence is deliberately
     * excluded; admission is NOT proof of DMA completion or audibility. */
    const int64_t admitted_at_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    s_metrics.pcm_packets_submitted++;
    s_metrics.pcm_bytes_submitted += (uint64_t)pkt->len;
    s_metrics.last_pcm_submit_monotonic_us = admitted_at_us;
    portEXIT_CRITICAL(&s_lock);
    s->last_played_ts = pkt->timestamp;
    s->playhead = (uint16_t)(pkt->seq + 1);
    michi_jb_release(&s->jb, pkt);
    metrics_live(s);
    return true;
}

/* ------------------------------------------------------------------
 * Session task (owns the socket and the jitter buffer; self-deletes)
 * ------------------------------------------------------------------ */

/* Session self-end failures go through michi_state_report_error - the
 * cause is captured directly (guaranteed under the state mux) and the
 * bus broadcast is best-effort. Called ONLY from the session self-end
 * paths (never on stop() teardown). */
static void session_post_error(esp_err_t data)
{
    (void)michi_state_report_error(MICHI_EVENT_ERROR, (uint32_t)data);
}

/* H14: ownership of the socket and PSRAM allocations is transactional.
 * Only the task or the failed start path releases the context, never both. */
static void session_context_release(session_t *s)
{
    if (s == NULL) return;
    if (s->sock >= 0) close(s->sock);
    free(s->jb.entries);
    free(s->jb.pool);
    free(s->recv_buf);
    free(s->zeros);
    free(s);
}

static void session_task(void *arg)
{
    session_t *s = (session_t *)arg;
    /* H14 startup handshake: xTaskCreate is not a publication barrier.
     * The creator first owns/publishes the handle and then releases this
     * task. Thus no self-delete can precede handle publication. */
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    bool self_end = false;  /* the task ended by itself, not via stop() */
    bool was_paused = false;

    if (!s_session_run) {
        /* stop() raced with task creation: never touch the pipeline. */
        goto out;
    }

    /* Prefill before the first write (deadline-bounded: on expiry the
     * session starts with whatever arrived - logged). */
    session_fill(s, MICHI_AUDIO_PREFILL_DEADLINE_MS);
    if (!s_session_run) {
        goto out;
    }
    if (s->jb.count < prefill_target(s)) {
        ESP_LOGW(TAG, "prefill deadline expired: starting playback with "
                      "%u of %u packets in buffer",
                 (unsigned)s->jb.count, (unsigned)prefill_target(s));
    }

    while (s_session_run) {
        session_recv(s);
        /* H15: action is sampled AFTER the blocking receive. `volatile`
         * or an atomic load before the wait cannot make it current here. */
        const michi_audio_worker_action_t action =
            michi_audio_worker_next_action(s_session_run, s_paused);
        if (action == MICHI_AUDIO_WORKER_STOP) {
            break; /* stop request or terminal socket error */
        }
        if (action == MICHI_AUDIO_WORKER_PAUSE) {
            /* Paused: receive + count, discard. Flush any buffered packets
             * so no stale audio lingers. */
            if (s->jb.count > 0) {
                jb_flush(&s->jb);
            }
            was_paused = true;
            continue;
        }
        if (was_paused) {
            was_paused = false;
            jb_flush(&s->jb);
            s->stream_seeded = false;
            s->last_played_ts = 0;
            s->in_underrun = false;
            m_set(&s_metrics.provisionally_missing, 0);
            metrics_live(s);
            session_fill(s, MICHI_AUDIO_PREFILL_DEADLINE_MS);
            continue;
        }
        if (!session_drain(s)) {
            if (!s_session_run) {
                break; /* STOP interrupted a blocking PCM write */
            }
            if (michi_audio_pause_interrupted_pcm(
                    s_session_run, s_paused,
                    michi_audio_output_is_quiesced())) {
                /* PAUSE won the race with a previously admitted PCM write.
                 * Treat the stopped producer as PAUSE, never as a dead session. */
                was_paused = true;
                jb_flush(&s->jb);
                metrics_live(s);
                continue;
            }
            ESP_LOGE(TAG, "session: pipeline rejected a write - ending session");
            session_post_error(ESP_ERR_INVALID_STATE);
            s_session_run = false;
            self_end = true;
            break;
        }
        if (s->stream_seeded && s->jb.count == 0) {
            /* Underrun: explicit silence keeps the clocks running, then
             * a brief re-prefill resyncs the playhead to the next
             * packet. */
            if (!s->in_underrun) {
                s->in_underrun = true;
                ESP_LOGW(TAG, "underrun: jitter buffer empty - explicit "
                              "silence + brief re-prefill");
                m_add(&s_metrics.underruns, 1);
            }
            if (!write_silence(s, s->samples_per_packet)) {
                if (!s_session_run) break;
                if (michi_audio_pause_interrupted_pcm(
                    s_session_run, s_paused,
                    michi_audio_output_is_quiesced())) {
                    was_paused = true;
                    jb_flush(&s->jb);
                    metrics_live(s);
                    continue;
                }
                session_post_error(ESP_ERR_INVALID_STATE);
                s_session_run = false;
                self_end = true;
                break;
            }
            const uint32_t recovery_deadline = michi_audio_recovery_deadline_ms(s->buffer_ms);
            session_fill(s, recovery_deadline);
            if (!s_session_run) {
                break; /* terminal UDP socket error during re-prefill */
            }
            const uint32_t recovery_stale =
                michi_jb_discard_stale(&s->jb, s->playhead);
            if (recovery_stale != 0) {
                m_add(&s_metrics.late, recovery_stale);
                metrics_live(s);
            }
            if (s->jb.count < prefill_target(s)) {
                ESP_LOGW(TAG, "underrun: degraded recovery - filled %u/%u packets within %u ms deadline",
                         (unsigned)s->jb.count, (unsigned)prefill_target(s), (unsigned)recovery_deadline);
            }
            if (s->jb.count > 0) {
                jb_entry_t *oldest = jb_oldest(&s->jb, s->playhead);
                if (oldest != NULL) {
                    if (oldest->seq != s->playhead) {
                        const uint32_t skipped = (uint32_t)(uint16_t)(oldest->seq - s->playhead);
                        m_add(&s_metrics.lost, skipped);
                        m_sub(&s_metrics.provisionally_missing, skipped);
                    }
                    s->playhead = oldest->seq; /* resync: no gap silence */
                    s->last_played_ts = 0;
                }
                s->in_underrun = false;
            }
        }
    }

out:
    if (s->fatal_recv_error) {
        session_post_error(ESP_FAIL);
        self_end = true;
    }
    if (self_end) {
        s_session_run = false;
        /* The session ended BY ITSELF (pipeline write failure): clear
         * the active flag so session_start() can open a new session.
         * When stop() initiated the teardown it clears the flag after
         * the join; this local flag guarantees we never stomp a NEW
         * session's active bit (the clear happens strictly before the
         * done flag, which is what a new session_start() waits on). */
        portENTER_CRITICAL(&s_lock);
        s_session_active = false;
        portEXIT_CRITICAL(&s_lock);
    }
    session_context_release(s);
    /* Cooperative: the task releases its own resources and signals, then
     * self-deletes. stop() never vTaskDelete()s from the outside. */
    s_session_done = true;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------
 * Output ops (the abstraction required by the spec; concrete impl:
 * michi_audio_output + michi_volume)
 * ------------------------------------------------------------------ */

static esp_err_t output_ops_prepare(uint32_t sample_rate, uint8_t bit_depth,
                                    uint8_t channels)
{
    if (sample_rate == MICHI_AUDIO_SAMPLE_RATE && bit_depth == MICHI_AUDIO_BIT_DEPTH &&
        channels == MICHI_AUDIO_CHANNELS) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "prepare: %" PRIu32 "/%u/%u not supported (canonical "
                  "profile is %d/%d/%d)",
             sample_rate, bit_depth, channels, MICHI_AUDIO_SAMPLE_RATE,
             MICHI_AUDIO_BIT_DEPTH, MICHI_AUDIO_CHANNELS);
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t output_ops_start(void)
{
    if (!michi_audio_output_is_running()) {
        return michi_audio_output_start();
    }
    return ESP_OK; /* boot_dac keeps the pipeline running; idempotent */
}

static esp_err_t output_ops_write(const uint8_t *data, size_t len)
{
    return michi_audio_output_write(data, len);
}

static esp_err_t output_ops_set_volume(uint8_t volume)
{
    return michi_volume_set(volume);
}

static esp_err_t output_ops_mute(bool mute)
{
    /* Mute is orthogonal to volume: do not destroy the user's level.
     * PCM is gated to zero for BOTH hardware- and software-volume DACs. */
    michi_volume_set_muted(mute);
    return ESP_OK;
}

static esp_err_t output_ops_stop(void)
{
    return michi_audio_output_stop();
}

const michi_audio_output_ops_t *michi_audio_get_output_ops(void)
{
    static const michi_audio_output_ops_t s_ops = {
        .prepare = output_ops_prepare,
        .start = output_ops_start,
        .write = output_ops_write,
        .set_volume = output_ops_set_volume,
        .mute = output_ops_mute,
        .stop = output_ops_stop,
    };
    return &s_ops;
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

esp_err_t michi_audio_init(void)
{
    if (s_initialized) {
        return ESP_OK; /* idempotent */
    }
    /* The Kconfig range pins MICHI_AUDIO_RTP_PT_S16LE to 97 at BUILD
     * time (a stale sdkconfig carrying another PT fails configuration,
     * never silently ships a non-canonical device). */
    if (MICHI_AUDIO_RX_BUF_BYTES < (int)MICHI_AUDIO_RTP_PAYLOAD_BYTES + 12) {
        ESP_LOGE(TAG, "init: RX buffer %d is smaller than the canonical "
                      "12-byte header + %u-byte payload",
                 MICHI_AUDIO_RX_BUF_BYTES,
                 (unsigned)MICHI_AUDIO_RTP_PAYLOAD_BYTES);
        return ESP_ERR_INVALID_ARG;
    }
    if (CONFIG_MICHI_AUDIO_JITTER_MAX_MS < MICHI_AUDIO_PACKET_MS_ASSUMED) {
        ESP_LOGE(TAG, "init: MICHI_AUDIO_JITTER_MAX_MS=%d is below one packet",
                 CONFIG_MICHI_AUDIO_JITTER_MAX_MS);
        return ESP_ERR_INVALID_ARG;
    }
    if (michi_audio_check_capacity_invariant(CONFIG_MICHI_AUDIO_JITTER_MAX_MS, MICHI_AUDIO_BUFFER_MS_MAX) != ESP_OK) {
        ESP_LOGE(TAG, "init: CONFIG_MICHI_AUDIO_JITTER_MAX_MS=%d < MICHI_AUDIO_BUFFER_MS_MAX=%d violates contract",
                 CONFIG_MICHI_AUDIO_JITTER_MAX_MS, MICHI_AUDIO_BUFFER_MS_MAX);
        return ESP_ERR_INVALID_STATE;
    }
    if (CONFIG_MICHI_AUDIO_PREFILL_MS > CONFIG_MICHI_AUDIO_JITTER_MAX_MS) {
        ESP_LOGW(TAG, "init: prefill %d ms > jitter capacity %d ms - prefill "
                      "clamped at runtime",
                 CONFIG_MICHI_AUDIO_PREFILL_MS, CONFIG_MICHI_AUDIO_JITTER_MAX_MS);
    }
    ESP_LOGI(TAG, "init: pt=%d payload=%u bytes jitter=%d ms (%d packets) "
                  "prefill=%d ms rx_buf=%d stack=%d",
             CONFIG_MICHI_AUDIO_RTP_PT_S16LE,
             (unsigned)MICHI_AUDIO_RTP_PAYLOAD_BYTES,
             CONFIG_MICHI_AUDIO_JITTER_MAX_MS, MICHI_AUDIO_MAX_PACKETS,
             CONFIG_MICHI_AUDIO_PREFILL_MS, MICHI_AUDIO_RX_BUF_BYTES,
             CONFIG_MICHI_AUDIO_TASK_STACK_BYTES);
    s_initialized = true;
    return ESP_OK;
}

/* Create the UDP socket and bind it: port 0 = the receiver picks a free
 * port in 49152..65535 (random start, full sweep). Returns the socket fd
 * (>= 0) with *out_port set, or -1 with the socket already closed. */
static int session_bind_socket(uint16_t port, uint16_t *out_port)
{
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "session: socket() failed (errno %d)", errno);
        return -1;
    }
    struct timeval tv = {
        .tv_sec = 0,
        .tv_usec = MICHI_AUDIO_SESSION_SOCKET_TIMEOUT_MS * 1000,
    };
    /* Without a bounded recvfrom() wait the session task could block forever
     * after stop(): fail CLOSED before a task or externally visible session
     * can exist. A failed timeout option must never silently be ignored. */
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        ESP_LOGE(TAG, "session: SO_RCVTIMEO setup failed (errno %d)", errno);
        close(sock);
        return -1;
    }

    if (port != 0) {
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            *out_port = port;
            return sock;
        }
        ESP_LOGE(TAG, "session: bind :%u failed (errno %d)",
                 (unsigned)port, errno);
        close(sock);
        return -1;
    }

    const uint32_t span = MICHI_AUDIO_STREAM_PORT_MAX -
                          MICHI_AUDIO_STREAM_PORT_MIN + 1;
    const uint32_t start = MICHI_AUDIO_STREAM_PORT_MIN + (esp_random() % span);
    for (uint32_t i = 0; i < span; i++) {
        const uint16_t cand = (uint16_t)(MICHI_AUDIO_STREAM_PORT_MIN +
            ((start - MICHI_AUDIO_STREAM_PORT_MIN + i) % span));
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(cand);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            *out_port = cand;
            return sock;
        }
    }
    ESP_LOGE(TAG, "session: no free UDP port in %d..%d",
             MICHI_AUDIO_STREAM_PORT_MIN, MICHI_AUDIO_STREAM_PORT_MAX);
    close(sock);
    return -1;
}

static esp_err_t michi_audio_session_start_impl(uint32_t port, uint32_t ssrc,
                                    const char *source_ip, uint16_t buffer_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (michi_audio_validate_buffer_ms(buffer_ms) != ESP_OK) {
        ESP_LOGW(TAG, "session start: buffer_ms %u outside %d..%d",
                 (unsigned)buffer_ms, MICHI_AUDIO_BUFFER_MS_MIN,
                 MICHI_AUDIO_BUFFER_MS_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    if (ssrc == 0) {
        ESP_LOGW(TAG, "session start: SSRC 0 is not negotiable");
        return ESP_ERR_INVALID_ARG;
    }
    if (source_ip == NULL || source_ip[0] == '\0') {
        ESP_LOGW(TAG, "session start: missing source IP");
        return ESP_ERR_INVALID_ARG;
    }
    if (port != 0 &&
        (port < MICHI_AUDIO_STREAM_PORT_MIN ||
         port > MICHI_AUDIO_STREAM_PORT_MAX)) {
        ESP_LOGW(TAG, "session start: port %u outside %d..%d",
                 (unsigned)port, MICHI_AUDIO_STREAM_PORT_MIN,
                 MICHI_AUDIO_STREAM_PORT_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    ip4_addr_t parsed_ip;
    if (ip4addr_aton(source_ip, &parsed_ip) == 0) {
        ESP_LOGW(TAG, "session start: source IP '%s' is not a dotted IPv4",
                 source_ip);
        return ESP_ERR_INVALID_ARG;
    }
    struct in_addr peer;
    peer.s_addr = parsed_ip.addr;
    if (s_session_task != NULL) {
        if (!s_session_done) {
            ESP_LOGE(TAG, "session start: a session task already exists");
            return ESP_ERR_INVALID_STATE;
        }
        /* Previous worker self-ended. Its completed generation can now
         * be retired before opening another session. */
        s_session_task = NULL;
        s_session_done = false;
    }
    if (s_session_active) {
        ESP_LOGE(TAG, "session start: a session is already active");
        return ESP_ERR_INVALID_STATE;
    }
    /* An earlier worker may have self-ended before start() could publish
     * its handle. Once the task is observed finished and unowned, consume
     * the stale completion signal before launching a new generation. */
    if (s_session_task == NULL && s_session_done) {
        s_session_done = false;
    }
    const michi_audio_output_state_t output_state = michi_audio_output_get_state();
    if (output_state != MICHI_AUDIO_STATE_RUNNING &&
        output_state != MICHI_AUDIO_STATE_QUIESCED &&
        output_state != MICHI_AUDIO_STATE_FAULTED &&
        output_state != MICHI_AUDIO_STATE_STOPPING) {
        ESP_LOGE(TAG, "session start: audio output not prepared (%d)",
                 (int)output_state);
        return ESP_ERR_INVALID_STATE;
    }
    /* H14: allocate/bind UDP resources before touching the I2S pipeline.
     * A failed socket bind or PSRAM allocation must not restart the DAC. */
    session_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s == NULL) {
        ESP_LOGE(TAG, "session start: session context allocation failed");
        return ESP_ERR_NO_MEM;
    }
    s->port = port;
    s->ssrc = ssrc;
    s->buffer_ms = buffer_ms;
    s->sock = -1;
    s->samples_per_packet = MICHI_AUDIO_SAMPLES_PER_PACKET;
    s->guard.pt = (uint8_t)MICHI_AUDIO_RTP_PT_S16LE;
    s->guard.ssrc = ssrc;
    s->guard.source_be32 = peer.s_addr;
    s->jb.entries = heap_caps_calloc(MICHI_AUDIO_MAX_PACKETS, sizeof(jb_entry_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s->jb.pool = heap_caps_calloc((size_t)MICHI_AUDIO_MAX_PACKETS * MICHI_AUDIO_JB_SLOT_BYTES,
                                  1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s->recv_buf = heap_caps_malloc(MICHI_AUDIO_RX_BUF_BYTES,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s->zeros = heap_caps_calloc(MICHI_AUDIO_JB_SLOT_BYTES, 1,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s->jb.entries == NULL || s->jb.pool == NULL || s->recv_buf == NULL ||
        s->zeros == NULL) {
        ESP_LOGE(TAG, "session start: buffer allocation failed "
                      "(%d packets x %d PCM bytes; UDP rx=%d bytes)",
                 MICHI_AUDIO_MAX_PACKETS, MICHI_AUDIO_JB_SLOT_BYTES,
                 MICHI_AUDIO_RX_BUF_BYTES);
        session_context_release(s);
        return ESP_ERR_NO_MEM;
    }
    /* Host-side JB tests enforce that copied packet sizes fit the stride;
     * the receiver already validates the precise PCM geometry upstream. */
    michi_jb_init(&s->jb, s->jb.entries, s->jb.pool, MICHI_AUDIO_MAX_PACKETS,
                  MICHI_AUDIO_JB_SLOT_BYTES);

    /* Bind BEFORE any task exists: a bind/socket failure is reported to
     * the caller and NOTHING of the session survives - the caller rolls
     * back to idle (all-or-nothing, no phantom session). */
    uint16_t bound = 0;
    s->sock = session_bind_socket(port, &bound);
    if (s->sock < 0) {
        session_context_release(s);
        return ESP_FAIL;
    }

    /* Commit point: prepared socket/memory exists, but session is not
     * visible. Only now reconcile a prior FAULTED output and restart I2S.
     * On failure we release all network resources without phantom state. */
    esp_err_t err = michi_audio_output_stop();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "session start: pipeline stop failed: %s",
                 esp_err_to_name(err));
        session_context_release(s);
        return err;
    }
    err = michi_audio_output_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "session start: pipeline restart failed: %s",
                 esp_err_to_name(err));
        session_context_release(s);
        return err;
    }
    portENTER_CRITICAL(&s_lock);
    memset(&s_metrics, 0, sizeof(s_metrics));
    s_session_ssrc = ssrc;
    s_session_peer = peer;
    s_paused = false;
    s_session_port = bound;
    /* Do not advertise an active session until the task handle is
     * published. A task-creation failure must never be externally visible
     * as an established receiver session. */
    portEXIT_CRITICAL(&s_lock);
    s_session_run = true;
    TaskHandle_t task = NULL;
    if (xTaskCreate(session_task, "michi_session", CONFIG_MICHI_AUDIO_TASK_STACK_BYTES,
                    s, MICHI_AUDIO_SESSION_TASK_PRIO, &task) != pdPASS) {
        ESP_LOGE(TAG, "session start: task creation failed");
        s_session_run = false;
        portENTER_CRITICAL(&s_lock);
        s_session_active = false;
        portEXIT_CRITICAL(&s_lock);
        session_context_release(s);
        /* No RTP producer exists. Quiesce I2S so the failed start does not
         * advertise a playing pipeline. Preserve the original failure. */
        esp_err_t quiet_err = michi_audio_output_quiesce();
        if (quiet_err != ESP_OK) {
            ESP_LOGE(TAG, "session: rollback silence failed: %s",
                     esp_err_to_name(quiet_err));
            (void)michi_audio_output_stop();
        }
        return ESP_ERR_NO_MEM;
    }
    /* H14: task remains blocked on its startup notification until the
     * creating thread has published a valid handle and an active session.
     * No worker failure can interleave with this publication sequence. */
    s_session_task = task;
    portENTER_CRITICAL(&s_lock);
    s_session_active = true;
    portEXIT_CRITICAL(&s_lock);
    xTaskNotifyGive(task);
    ESP_LOGI(TAG, "session: udp :%u ssrc=0x%08" PRIx32 " peer=%s buffer_ms=%u",
             (unsigned)bound, ssrc, source_ip, (unsigned)buffer_ms);
    return ESP_OK;
}

static esp_err_t michi_audio_session_stop_impl(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_session_task == NULL && !s_session_active) {
        return ESP_OK; /* idempotent */
    }
    s_session_run = false;
    /* Never discard a failed silence barrier. Caller must be told if the
     * output remains FAULTED even though the network task was joined. */
    const esp_err_t quiesce_err = michi_audio_output_quiesce();
    /* The task wakes within its 100 ms socket timeout or after the
     * completed blocking ring write; it tears down its own resources. */
    int waited_ms = 0;
    while (!s_session_done && waited_ms < MICHI_AUDIO_JOIN_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
    }
    if (!s_session_done) {
        ESP_LOGE(TAG, "session stop: task did not self-delete within %d ms - "
                      "retry stop()",
                 MICHI_AUDIO_JOIN_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }
    s_session_task = NULL;
    s_session_done = false;
    s_session_active = false; /* the task is gone: the session is over */
    portENTER_CRITICAL(&s_lock);
    s_paused = false;
    portEXIT_CRITICAL(&s_lock);
    if (quiesce_err != ESP_OK && quiesce_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "session stopped, but output silence failed: %s",
                 esp_err_to_name(quiesce_err));
        return quiesce_err;
    }
    /* An INVALID_STATE during shutdown can mean the output was FAULTED,
     * not a successful silence operation. Preserve that signal. */
    if (quiesce_err != ESP_OK &&
        michi_audio_output_get_state() == MICHI_AUDIO_STATE_FAULTED) {
        ESP_LOGE(TAG, "session stopped with FAULTED output; next start must reconcile");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "session: stopped");
    return ESP_OK;
}

/* Pause implementation is below the public transition wrappers. */
static esp_err_t michi_audio_session_set_paused_impl(bool paused);

/* H13 public transition gates. No protected spinlock spans driver I/O,
 * blocking socket waits or command ACKs. A failed lease is retryable. */
esp_err_t michi_audio_session_start(uint32_t port, uint32_t ssrc,
                                    const char *source_ip, uint16_t buffer_ms)
{
    if (!session_lifecycle_try_enter()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = michi_audio_session_start_impl(port, ssrc, source_ip, buffer_ms);
    session_lifecycle_leave();
    return err;
}

esp_err_t michi_audio_session_stop(void)
{
    if (!session_lifecycle_try_enter()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = michi_audio_session_stop_impl();
    session_lifecycle_leave();
    return err;
}

esp_err_t michi_audio_session_set_paused(bool paused)
{
    if (!session_lifecycle_try_enter()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = michi_audio_session_set_paused_impl(paused);
    session_lifecycle_leave();
    return err;
}

bool michi_audio_session_active(void)
{
    return s_session_active;
}

static esp_err_t michi_audio_session_set_paused_impl(bool paused)
{
    if (!s_session_active) {
        return ESP_ERR_INVALID_STATE;
    }
    /* H15: mark PAUSE admission BEFORE blocking on the QUIESCE ACK.
     * Worker checks this flag after recvfrom() and at failed PCM writes.
     * RESUME keeps it true until the I2S accept barrier opens. */
    if (paused) s_paused = true;
    esp_err_t err = paused ? michi_audio_output_quiesce()
                           : michi_audio_output_resume();
    if (err == ESP_OK) {
        if (!paused) s_paused = false;
    } else if (paused) {
        /* A failed pause cannot be reported as an accepted PAUSED state.
         * A FAULTED I2S worker still fails closed in the write path. */
        s_paused = false;
    }
    return err;
}

esp_err_t michi_audio_session_get_port(uint16_t *out_port)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_port == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_session_active) {
        return ESP_ERR_NOT_FOUND;
    }
    portENTER_CRITICAL(&s_lock);
    *out_port = s_session_port;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t michi_audio_session_get_ssrc(uint32_t *out_ssrc)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_ssrc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_session_active) {
        return ESP_ERR_NOT_FOUND;
    }
    portENTER_CRITICAL(&s_lock);
    *out_ssrc = s_session_ssrc; /* the negotiated value, set at start */
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t michi_audio_session_get_peer(char *out, size_t out_len)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_session_active) {
        return ESP_ERR_NOT_FOUND;
    }
    struct in_addr peer;
    portENTER_CRITICAL(&s_lock);
    peer = s_session_peer;
    portEXIT_CRITICAL(&s_lock);
    ip4_addr_t peer_ip;
    peer_ip.addr = peer.s_addr;
    if (ip4addr_ntoa_r(&peer_ip, out, (int)out_len) == NULL) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t michi_audio_get_metrics(michi_audio_metrics_t *out)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_metrics;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t michi_audio_boot_dac(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    const michi_dac_caps_t *caps = michi_dac_get_caps();
    if (!caps->detected) {
        ESP_LOGW(TAG, "boot_dac: no DAC detected - I2S clocks not started, "
                      "profile stays diagnostic");
        return ESP_OK; /* honest: nothing to boot */
    }
    if (caps->initialized) {
        ESP_LOGI(TAG, "boot_dac: DAC already initialized - I2S clocks not started");
        return ESP_OK;
    }

    if (!michi_audio_output_is_running()) {
        const michi_board_external_pins_t *pins = michi_board_get_external_pins();
        const michi_audio_output_config_t cfg = {
            .sample_rate = MICHI_AUDIO_SAMPLE_RATE,
            .bit_depth = MICHI_AUDIO_BIT_DEPTH,
            .channels = MICHI_AUDIO_CHANNELS,
            .buffer_ms = 20,
            .ring_buffer_kb = 0, /* Kconfig default (1024 KiB, PSRAM) */
            .bclk = pins->i2s_bclk,
            .lrck = pins->i2s_lrck,
            .din = pins->i2s_din,
            .mclk = pins->i2s_mclk,
        };
        esp_err_t err = michi_audio_output_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "boot_dac: audio output init failed: %s",
                     esp_err_to_name(err));
            return err;
        }
        err = michi_audio_output_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "boot_dac: audio output start failed: %s",
                     esp_err_to_name(err));
            michi_audio_output_deinit();
            return err;
        }
        ESP_LOGI(TAG, "boot_dac: I2S running at %d/%d/%d (silence, ring "
                      "auto-clears)",
                 MICHI_AUDIO_SAMPLE_RATE, MICHI_AUDIO_BIT_DEPTH,
                 MICHI_AUDIO_CHANNELS);
    } else {
        ESP_LOGI(TAG, "boot_dac: pipeline already running (retry path)");
    }

    /* With BCLK/LRCK continuous the PCM5122 PLL can lock: re-run the
     * start for the canonical profile. Honest on failure: the error is
     * propagated and the profile stays diagnostic; the clocks are left
     * running so a later retry works. */
    const esp_err_t err = michi_dac_start(MICHI_AUDIO_SAMPLE_RATE,
                                          MICHI_AUDIO_BIT_DEPTH,
                                          MICHI_AUDIO_CHANNELS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "boot_dac: DAC still NOT initialized (%s) - profile stays "
                      "diagnostic, I2S clocks left running",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "boot_dac: DAC initialized with I2S clocks");
    }

    michi_product_profile_refresh();
    ESP_LOGI(TAG, "audio tier after DAC boot: %s", michi_product_profile_tier_name());
    return err;
}
