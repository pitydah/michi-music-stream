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
 *    overrun (drop-oldest), discontinuity (flush + resync). Sequence
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
#define MICHI_AUDIO_JITTER_SAMPLE_CLAMP_US 1000000 /* 1 s: sender stalls are not jitter */

/* Jitter buffer capacity in packets, in 10 ms units (spec convention). */
#define MICHI_AUDIO_MAX_PACKETS (CONFIG_MICHI_AUDIO_JITTER_MAX_MS / \
                                 MICHI_AUDIO_PACKET_MS_ASSUMED)

#define MICHI_AUDIO_RX_BUF_BYTES CONFIG_MICHI_AUDIO_RX_BUF_BYTES

typedef struct {
    uint16_t seq;      /* RTP sequence */
    uint32_t timestamp; /* RTP timestamp */
    uint16_t len;      /* payload bytes (canonical: 1920) */
    bool     used;
} jb_entry_t;

typedef struct {
    jb_entry_t *entries;      /* MICHI_AUDIO_MAX_PACKETS descriptors */
    uint8_t    *pool;         /* MICHI_AUDIO_MAX_PACKETS * RX_BUF bytes (PSRAM) */
    uint32_t    count;        /* packets currently buffered */
} jitter_buffer_t;

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
    uint32_t base_ts;        /* first packet ts (jitter reference) */
    int64_t  base_time_us;   /* first packet arrival (jitter reference) */
    uint32_t jitter_us;      /* EWMA estimate */
    bool     in_underrun;    /* one underrun counted per contiguous stall */
    uint32_t drop_log_count; /* rogue-source log throttle */

    jitter_buffer_t jb;
    uint8_t *recv_buf;       /* datagram buffer (heap) */
    uint8_t *zeros;          /* one packet of silence (heap) */
} session_t;

static volatile bool s_initialized = false;
static volatile bool s_session_run = false;    /* cooperative: read by the task */
static volatile bool s_session_done = false;   /* set by the task before self-delete */
static volatile bool s_session_active = false; /* session bound and running */
static volatile TaskHandle_t s_session_task = NULL;
static volatile bool s_paused = false;         /* pause flag read by the task */

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

static void m_set(uint32_t *field, uint32_t v)
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

static jb_entry_t *jb_slot_by_index(jitter_buffer_t *jb, uint32_t idx)
{
    return &jb->entries[idx];
}

static uint32_t jb_index(const jitter_buffer_t *jb, const jb_entry_t *e)
{
    return (uint32_t)(e - jb->entries);
}

static jb_entry_t *jb_find_seq(jitter_buffer_t *jb, uint16_t seq)
{
    for (uint32_t i = 0; i < MICHI_AUDIO_MAX_PACKETS; i++) {
        if (jb->entries[i].used && jb->entries[i].seq == seq) {
            return &jb->entries[i];
        }
    }
    return NULL;
}

/* Oldest pending packet: the entry with the smallest non-negative
 * (int16_t)(seq - playhead). NULL when the buffer is empty. */
static jb_entry_t *jb_oldest(jitter_buffer_t *jb, uint16_t playhead)
{
    jb_entry_t *best = NULL;
    int16_t best_diff = INT16_MAX;
    for (uint32_t i = 0; i < MICHI_AUDIO_MAX_PACKETS; i++) {
        jb_entry_t *e = &jb->entries[i];
        if (!e->used) {
            continue;
        }
        int16_t diff = (int16_t)(e->seq - playhead);
        if (diff >= 0 && diff < best_diff) {
            best = e;
            best_diff = diff;
        }
    }
    return best;
}

static void jb_flush(jitter_buffer_t *jb)
{
    for (uint32_t i = 0; i < MICHI_AUDIO_MAX_PACKETS; i++) {
        jb->entries[i].used = false;
    }
    jb->count = 0;
}

/* Insert a copy of the payload. Returns ESP_ERR_INVALID_STATE when the seq
 * is already queued (caller already classified it as duplicate); applies
 * the drop-oldest overrun policy (oldest relative to the playhead) when the
 * buffer is full. Both rejections (drop-oldest and NO_MEM) count as
 * overruns and are logged (DROP_LOG throttles a rogue sender's spam). */
static esp_err_t jb_insert(session_t *s, uint16_t playhead,
                           const michi_rtp_guard_packet_t *pkt)
{
    jitter_buffer_t *jb = &s->jb;
    if (jb_find_seq(jb, pkt->seq) != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (jb->count >= MICHI_AUDIO_MAX_PACKETS) {
        jb_entry_t *oldest = jb_oldest(jb, playhead);
        if (oldest == NULL) {
            DROP_LOG(s, "jitter buffer full but no evictable packet: "
                        "seq=%u dropped", (unsigned)pkt->seq);
            m_add(&s_metrics.overruns, 1);
            return ESP_ERR_NO_MEM; /* defensive: count said full */
        }
        ESP_LOGW(TAG, "jitter buffer full: dropped oldest seq=%u",
                 (unsigned)oldest->seq);
        m_add(&s_metrics.overruns, 1);
        oldest->used = false;
        jb->count--;
    }
    for (uint32_t i = 0; i < MICHI_AUDIO_MAX_PACKETS; i++) {
        jb_entry_t *slot = jb_slot_by_index(jb, i);
        if (!slot->used) {
            memcpy(jb->pool + (size_t)i * MICHI_AUDIO_RX_BUF_BYTES,
                   pkt->payload, pkt->payload_len);
            slot->seq = pkt->seq;
            slot->timestamp = pkt->timestamp;
            slot->len = pkt->payload_len;
            slot->used = true;
            jb->count++;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM; /* defensive: count said there was room */
}

/* ------------------------------------------------------------------
 * Stream policy (see header for the full taxonomy)
 * ------------------------------------------------------------------ */

static bool stream_policy(session_t *s, const michi_rtp_guard_packet_t *pkt)
{
    const int16_t max_pkts = (int16_t)MICHI_AUDIO_MAX_PACKETS;

    if (s_paused) {
        /* Paused: valid packets are counted (received + loss
         * accounting) but never queued - silence, not a buffer leak. */
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
        /* First accepted packet: seed the playhead and the jitter
         * reference. PT/SSRC/source/size were already validated by the
         * guard against the NEGOTIATED constants - the payload geometry
         * is canonical (1920 bytes = 480 samples). */
        s->stream_seeded = true;
        s->playhead = pkt->seq;
        s->last_seq = pkt->seq;
        s->base_ts = pkt->timestamp;
        s->base_time_us = esp_timer_get_time();
        (void)jb_insert(s, s->playhead, pkt);
        return true;
    }

    const int16_t diff_p = (int16_t)(pkt->seq - s->playhead);

    if (diff_p > max_pkts) {
        /* Ahead of the playhead by more than the window: stream
         * discontinuity (sender restart). Flush + resync; the buffered
         * packets are obsolete. */
        ESP_LOGW(TAG, "seq %u ahead of playhead %u by more than the window: "
                      "buffer flush + resync",
                 (unsigned)pkt->seq, (unsigned)s->playhead);
        jb_flush(&s->jb);
        s->playhead = pkt->seq;
        s->last_seq = pkt->seq; /* reset the received high-water mark */
        s->last_played_ts = 0;
        (void)jb_insert(s, s->playhead, pkt);
        return true;
    }
    if (diff_p < -max_pkts) {
        m_add(&s_metrics.late, 1); /* behind the playhead by > window */
        return false;
    }
    if (diff_p < 0) {
        /* Behind the playhead within the window: already
         * reproduced/passed. diff_p == 0 (seq == playhead) is the NEXT
         * EXPECTED packet - never dropped here: it is usually not queued
         * (it is the gap the buffer is waiting for), so jb_insert must
         * decide. jb_find_seq detects the real duplicate (INVALID_STATE
         * -> duplicate below). */
        m_add(&s_metrics.duplicate, 1); /* already reproduced/passed */
        return false;
    }

    const int16_t diff_l = (int16_t)(pkt->seq - s->last_seq);
    const uint32_t lost = michi_rtp_guard_lost_delta(s->last_seq, pkt->seq);
    if (lost != 0) {
        m_add(&s_metrics.lost, lost);
    }
    const bool reordered = (diff_l <= 0); /* out of order, still playable */

    const esp_err_t err = jb_insert(s, s->playhead, pkt);
    if (err == ESP_OK) {
        if (reordered) {
            m_add(&s_metrics.reordered, 1);
        }
        /* The received high-water mark only advances on in-order
         * arrivals; a reordered packet (diff_l <= 0) must never lower
         * it, or the next in-order packet would report fake loss. */
        if (diff_l > 0) {
            s->last_seq = pkt->seq;
        }
    } else if (err == ESP_ERR_INVALID_STATE) {
        m_add(&s_metrics.duplicate, 1);
        return false;
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
    if (n <= 0) {
        return; /* timeout (SO_RCVTIMEO) or error: keep the session alive */
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

    /* Jitter EWMA (no RTCP in this phase): expected arrival = first
     * arrival + (ts - base_ts) / sample_rate. uint64 accumulation (no
     * wrap of jitter_us*15 at ~4.8 min) and sample clamp (a sender
     * stall is not jitter). */
    const uint32_t ts_delta = pkt.timestamp - s->base_ts;
    const int64_t expected_us = s->base_time_us +
                                (int64_t)ts_delta * 1000000 / MICHI_AUDIO_SAMPLE_RATE;
    const int64_t sample_signed = esp_timer_get_time() - expected_us;
    uint64_t sample_us = sample_signed < 0 ? (uint64_t)(-sample_signed)
                                           : (uint64_t)sample_signed;
    if (sample_us > MICHI_AUDIO_JITTER_SAMPLE_CLAMP_US) {
        sample_us = MICHI_AUDIO_JITTER_SAMPLE_CLAMP_US;
    }
    s->jitter_us = (uint32_t)(((uint64_t)s->jitter_us * 15 + sample_us) / 16);
    m_set(&s_metrics.jitter_us, s->jitter_us);
}

/* ------------------------------------------------------------------
 * Playback path
 * ------------------------------------------------------------------ */

/* Prefill target in packets (ceil of prefill_ms / packet duration). */
static uint32_t prefill_target(const session_t *s)
{
    uint32_t prefill_ms = CONFIG_MICHI_AUDIO_PREFILL_MS;
    if (prefill_ms > CONFIG_MICHI_AUDIO_JITTER_MAX_MS) {
        prefill_ms = CONFIG_MICHI_AUDIO_JITTER_MAX_MS;
    }
    if (s->samples_per_packet == 0) {
        return 1; /* defensive: canonical geometry is fixed at 480 */
    }
    uint32_t packet_ms = (uint32_t)((uint64_t)s->samples_per_packet * 1000 /
                                    MICHI_AUDIO_SAMPLE_RATE);
    if (packet_ms == 0) {
        packet_ms = 1;
    }
    uint32_t target = (prefill_ms + packet_ms - 1) / packet_ms;
    return target > MICHI_AUDIO_MAX_PACKETS ? MICHI_AUDIO_MAX_PACKETS : target;
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
    jb_entry_t *pkt = jb_oldest(&s->jb, s->playhead);
    if (pkt == NULL) {
        return true; /* empty: the underrun path handles it */
    }

    if (pkt->seq != s->playhead) {
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
    if (michi_audio_output_write(s->jb.pool + (size_t)idx * MICHI_AUDIO_RX_BUF_BYTES,
                                 pkt->len) != ESP_OK) {
        return false;
    }
    s->last_played_ts = pkt->timestamp;
    s->playhead = (uint16_t)(pkt->seq + 1);
    pkt->used = false;
    s->jb.count--;
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

static void session_task(void *arg)
{
    session_t *s = (session_t *)arg;
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
        const bool paused = s_paused;
        session_recv(s);
        if (paused) {
            /* Paused: receive + count, discard. The playhead resync
             * happens on the first unpaused iteration below. */
            was_paused = true;
            continue;
        }
        if (was_paused) {
            was_paused = false;
            jb_flush(&s->jb);
            s->playhead = (uint16_t)(s->last_seq + 1);
            s->last_played_ts = 0;
            s->in_underrun = false;
            metrics_live(s);
        }
        if (!session_drain(s)) {
            ESP_LOGE(TAG, "session: pipeline rejected a write - ending session");
            session_post_error(ESP_ERR_INVALID_STATE);
            s_session_run = false;
            self_end = true;
            break;
        }
        if (s->jb.count == 0) {
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
                session_post_error(ESP_ERR_INVALID_STATE);
                self_end = true;
                break;
            }
            session_fill(s, MICHI_AUDIO_REPREFILL_MS);
            if (s->jb.count > 0) {
                jb_entry_t *oldest = jb_oldest(&s->jb, s->playhead);
                if (oldest != NULL) {
                    s->playhead = oldest->seq; /* resync: no gap silence */
                    s->last_played_ts = 0;
                }
                s->in_underrun = false;
            }
        }
    }

out:
    if (self_end) {
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
    if (s->sock >= 0) {
        close(s->sock);
    }
    free(s->jb.entries);
    free(s->jb.pool);
    free(s->recv_buf);
    free(s->zeros);
    free(s);
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
    if (mute) {
        return michi_volume_set(0); /* digital mute; get() reflects it */
    }
    return ESP_OK; /* restore with set_volume() (no stored pre-mute level) */
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
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

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

esp_err_t michi_audio_session_start(uint16_t port, uint32_t ssrc,
                                    const char *source_ip)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
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
    struct in_addr peer;
    if (ip4addr_aton(source_ip, &peer) == 0) {
        ESP_LOGW(TAG, "session start: source IP '%s' is not a dotted IPv4",
                 source_ip);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_session_task != NULL) {
        if (!s_session_done) {
            ESP_LOGE(TAG, "session start: a session task already exists");
            return ESP_ERR_INVALID_STATE;
        }
        /* Stale handle: the previous task self-deleted before
         * session_start assigned the handle. Clear it - the task is gone
         * (done flag observed). */
        s_session_task = NULL;
        s_session_done = false;
    }
    if (s_session_active) {
        ESP_LOGE(TAG, "session start: a session is already active");
        return ESP_ERR_INVALID_STATE;
    }
    if (!michi_audio_output_is_running()) {
        ESP_LOGE(TAG, "session start: audio pipeline not running "
                      "(michi_audio_boot_dac() must run at boot)");
        return ESP_ERR_INVALID_STATE;
    }
    /* Flush stale audio from a previous session: the pipeline start()
     * flushes the ring. The short clock gap between stop and start is
     * fine: the PCM5122 PLL re-locks from BCLK/LRCK within milliseconds. */
    esp_err_t err = michi_audio_output_stop();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "session start: pipeline stop failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    err = michi_audio_output_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "session start: pipeline restart failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    portENTER_CRITICAL(&s_lock);
    memset(&s_metrics, 0, sizeof(s_metrics)); /* fresh counters per session */
    s_session_ssrc = ssrc;  /* the NEGOTIATED source (no first-seen) */
    s_session_peer = peer;
    s_paused = false;
    portEXIT_CRITICAL(&s_lock);

    session_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s == NULL) {
        ESP_LOGE(TAG, "session start: session context allocation failed");
        return ESP_ERR_NO_MEM;
    }
    s->port = port;
    s->ssrc = ssrc;
    s->sock = -1;
    s->samples_per_packet = MICHI_AUDIO_SAMPLES_PER_PACKET;
    s->guard.pt = (uint8_t)MICHI_AUDIO_RTP_PT_S16LE;
    s->guard.ssrc = ssrc;
    s->guard.source_be32 = peer.s_addr;
    s->jb.entries = heap_caps_calloc(MICHI_AUDIO_MAX_PACKETS, sizeof(jb_entry_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s->jb.pool = heap_caps_calloc((size_t)MICHI_AUDIO_MAX_PACKETS * MICHI_AUDIO_RX_BUF_BYTES,
                                  1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s->recv_buf = heap_caps_malloc(MICHI_AUDIO_RX_BUF_BYTES,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s->zeros = heap_caps_calloc(MICHI_AUDIO_RX_BUF_BYTES, 1,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s->jb.entries == NULL || s->jb.pool == NULL || s->recv_buf == NULL ||
        s->zeros == NULL) {
        ESP_LOGE(TAG, "session start: buffer allocation failed "
                      "(%d packets x %d bytes pool)",
                 MICHI_AUDIO_MAX_PACKETS, MICHI_AUDIO_RX_BUF_BYTES);
        free(s->jb.entries);
        free(s->jb.pool);
        free(s->recv_buf);
        free(s->zeros);
        free(s);
        return ESP_ERR_NO_MEM;
    }

    /* Bind BEFORE any task exists: a bind/socket failure is reported to
     * the caller and NOTHING of the session survives - the caller rolls
     * back to idle (all-or-nothing, no phantom session). */
    uint16_t bound = 0;
    s->sock = session_bind_socket(port, &bound);
    if (s->sock < 0) {
        free(s->jb.entries);
        free(s->jb.pool);
        free(s->recv_buf);
        free(s->zeros);
        free(s);
        return ESP_FAIL;
    }

    portENTER_CRITICAL(&s_lock);
    s_session_port = bound;
    /* Active BEFORE the task exists: the invariant "active flag set =>
     * a live bound session" holds in both directions - a task that
     * self-ends clears the flag (strictly before its done flag), and a
     * failed task creation clears it here. */
    s_session_active = true;
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
        close(s->sock);
        free(s->jb.entries);
        free(s->jb.pool);
        free(s->recv_buf);
        free(s->zeros);
        free(s);
        return ESP_ERR_NO_MEM;
    }
    s_session_task = task;
    ESP_LOGI(TAG, "session: udp :%u ssrc=0x%08" PRIx32 " peer=%s",
             (unsigned)bound, ssrc, source_ip);
    return ESP_OK;
}

esp_err_t michi_audio_session_stop(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_session_task == NULL && !s_session_active) {
        return ESP_OK; /* idempotent */
    }
    s_session_run = false;
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
    ESP_LOGI(TAG, "session: stopped");
    return ESP_OK;
}

bool michi_audio_session_active(void)
{
    return s_session_active;
}

void michi_audio_session_set_paused(bool paused)
{
    portENTER_CRITICAL(&s_lock);
    s_paused = paused;
    portEXIT_CRITICAL(&s_lock);
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
    if (ip4addr_ntoa_r((const ip4_addr_t *)&peer, out, (int)out_len) == NULL) {
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
