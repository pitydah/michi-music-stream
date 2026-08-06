/*
 * RTP/UDP audio engine (phase 11, meta 1: PCM S16LE 48 kHz stereo).
 *
 * Design (see include/michi_audio.h for the full contract):
 *  - The session task owns the UDP socket and the jitter buffer; it is
 *    created by michi_audio_session_start() and self-deletes after
 *    cooperative shutdown (no external vTaskDelete).
 *  - Packet-level jitter buffer: ordered by 16-bit seq with wrap-aware
 *    int16 diff math; policies: duplicate, reordered, late, loss, overrun
 *    (drop-oldest), discontinuity (flush + resync).
 *  - Playback drains one packet per loop iteration; real-time pacing comes
 *    from the michi_audio_output ring (blocking write = ring flow control).
 *    Gaps and underruns write EXPLICIT zero samples: continuous BCLK/LRCK
 *    keeps the PCM5122 PLL locked (the ring's DMA auto-clear alone would
 *    also be silence, but explicit silence keeps the ring level stable).
 *  - Metrics are updated by the session task under a short portMUX critical
 *    section (single writer, arbitrary readers).
 *  - No 4 MB global buffer: bounded per-session PSRAM pool + lwip UDP
 *    receive mailbox (CONFIG_LWIP_UDP_RECVMBOX_SIZE raised in
 *    sdkconfig.defaults so bursts reach the jitter buffer).
 */

#include <inttypes.h>
#include <string.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "michi_audio.h"
#include "michi_audio_output.h"
#include "michi_board.h"
#include "michi_dac.h"
#include "michi_product_profile.h"
#include "michi_volume.h"

#define TAG "michi_audio"

/* Phase-11 validated profile (meta 1); the product profile keeps it as the
 * validation baseline. prepare() rejects anything else. */
#define MICHI_AUDIO_SAMPLE_RATE 48000
#define MICHI_AUDIO_BIT_DEPTH   16
#define MICHI_AUDIO_CHANNELS    2
#define MICHI_AUDIO_BYTES_PER_SAMPLE (MICHI_AUDIO_BIT_DEPTH / 8)

#define MICHI_AUDIO_RTP_VERSION 2
#define MICHI_AUDIO_PACKET_MS_ASSUMED 10 /* capacity unit: 10 ms packets (spec) */

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
    uint16_t len;      /* payload bytes (16-bit stereo: len % 4 == 0) */
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
    int      sock;
    uint16_t port;
    uint32_t ssrc_filter;

    /* Stream state */
    bool     ssrc_valid;
    uint32_t ssrc;
    uint16_t playhead;        /* next seq to play */
    uint16_t last_seq;        /* received high-water mark */
    uint32_t last_played_ts;  /* RTP ts of the last played packet; 0 = none */
    uint32_t samples_per_packet; /* from the first accepted payload */
    uint32_t base_ts;         /* first packet ts (jitter reference) */
    int64_t  base_time_us;    /* first packet arrival (jitter reference) */
    uint32_t jitter_us;       /* EWMA estimate */
    bool     in_underrun;     /* one underrun counted per contiguous stall */
    uint32_t drop_log_count;  /* rogue-source log throttle */

    jitter_buffer_t jb;
    uint8_t *recv_buf;        /* datagram buffer (heap) */
    uint8_t *zeros;           /* one packet of silence (heap) */
} session_t;

static volatile bool s_initialized = false;
static volatile bool s_session_run = false;    /* cooperative: read by the task */
static volatile bool s_session_done = false;   /* set by the task before self-delete */
static volatile bool s_session_active = false; /* task bound the socket */
static volatile TaskHandle_t s_session_task = NULL;

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
 * RTP parsing (RFC 3550 header: v/P/X/CC/M/PT/seq/timestamp/ssrc)
 * ------------------------------------------------------------------ */

typedef struct {
    uint8_t          pt;
    uint16_t         seq;
    uint32_t         timestamp;
    uint32_t         ssrc;
    const uint8_t   *payload;
    uint16_t         payload_len;
} rtp_packet_t;

static bool rtp_parse(const uint8_t *buf, size_t len, rtp_packet_t *out)
{
    if (len < 12) {
        return false;
    }
    if (((buf[0] >> 6) & 0x3) != MICHI_AUDIO_RTP_VERSION) {
        return false;
    }
    const bool padding = ((buf[0] >> 5) & 0x1) != 0;
    const bool extension = ((buf[0] >> 4) & 0x1) != 0;
    const uint8_t cc = buf[0] & 0x0F;

    size_t off = 12 + (size_t)cc * 4;
    if (extension) {
        if (off + 4 > len) {
            return false;
        }
        const uint16_t ext_len = ((uint16_t)buf[off + 2] << 8) | buf[off + 3];
        off += 4 + (size_t)ext_len * 4;
    }
    if (off > len) {
        return false;
    }
    uint16_t plen = (uint16_t)(len - off);
    if (padding) {
        if (plen == 0) {
            return false;
        }
        const uint16_t pad = buf[len - 1];
        if (pad > plen) {
            return false;
        }
        plen -= pad;
    }

    out->pt = buf[1] & 0x7F;
    out->seq = ((uint16_t)buf[2] << 8) | buf[3];
    out->timestamp = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                     ((uint32_t)buf[6] << 8) | buf[7];
    out->ssrc = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) |
                ((uint32_t)buf[10] << 8) | buf[11];
    out->payload = buf + off;
    out->payload_len = plen;
    return true;
}

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
                           const rtp_packet_t *pkt)
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

static bool stream_policy(session_t *s, const rtp_packet_t *pkt)
{
    const int16_t max_pkts = (int16_t)MICHI_AUDIO_MAX_PACKETS;

    if (!s->ssrc_valid) {
        /* First accepted packet: seed the stream (playhead, jitter ref,
         * packet geometry). Reject payloads that cannot be 16-bit stereo
         * BEFORE seeding. */
        if (pkt->payload_len == 0 || (pkt->payload_len % (MICHI_AUDIO_CHANNELS *
                                                          MICHI_AUDIO_BYTES_PER_SAMPLE)) != 0) {
            DROP_LOG(s, "first packet payload %u bytes: not 16-bit stereo aligned",
                     (unsigned)pkt->payload_len);
            m_add(&s_metrics.drops_payload_geometry, 1);
            return false; /* not accepted into the stream */
        }
        s->ssrc = pkt->ssrc;
        s->ssrc_valid = true;
        s->playhead = pkt->seq;
        s->last_seq = pkt->seq;
        s->base_ts = pkt->timestamp;
        s->base_time_us = esp_timer_get_time();
        s->samples_per_packet = pkt->payload_len /
                                (MICHI_AUDIO_CHANNELS * MICHI_AUDIO_BYTES_PER_SAMPLE);
        (void)jb_insert(s, s->playhead, pkt);
        return true;
    }

    /* Payload geometry is validated on EVERY packet, not just the first:
     * a zero/unaligned payload would reach write(0) -> INVALID_ARG and a
     * single datagram would kill the session. Drop it (counted per class,
     * logged, throttled) and keep the session alive. */
    if (pkt->payload_len == 0 || (pkt->payload_len % (MICHI_AUDIO_CHANNELS *
                                                      MICHI_AUDIO_BYTES_PER_SAMPLE)) != 0) {
        DROP_LOG(s, "payload %u bytes: not 16-bit stereo aligned - dropped, "
                    "session kept alive", (unsigned)pkt->payload_len);
        m_add(&s_metrics.drops_payload_geometry, 1);
        return false;
    }

    const int16_t diff_p = (int16_t)(pkt->seq - s->playhead);

    if (diff_p > max_pkts) {
        /* Ahead of the playhead by more than the window: stream
         * discontinuity (sender restart without SSRC change, or a reset
         * sender). Flush + resync; the buffered packets are obsolete. */
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
        /* Behind the playhead within the window: already reproduced/passed.
         * diff_p == 0 (seq == playhead) is the NEXT EXPECTED packet - never
         * dropped here: it is usually not queued (it is the gap the buffer
         * is waiting for), so jb_insert must decide. jb_find_seq detects the
         * real duplicate (INVALID_STATE -> duplicate below). Dropping it as
         * duplicate before would replace the expected packet with silence
         * (spurious start underrun / glitch on every post-underrun
         * recovery). */
        m_add(&s_metrics.duplicate, 1); /* already reproduced/passed */
        return false;
    }

    const int16_t diff_l = (int16_t)(pkt->seq - s->last_seq);
    if (diff_l > 1) {
        m_add(&s_metrics.lost, (uint32_t)(diff_l - 1));
    }
    const bool reordered = (diff_l <= 0); /* out of order, still playable */

    const esp_err_t err = jb_insert(s, s->playhead, pkt);
    if (err == ESP_OK) {
        if (reordered) {
            m_add(&s_metrics.reordered, 1);
        }
        /* The received high-water mark only advances on in-order arrivals;
         * a reordered packet (diff_l <= 0) must never lower it, or the
         * next in-order packet would report fake loss. */
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

    rtp_packet_t pkt;
    if (!rtp_parse(s->recv_buf, (size_t)n, &pkt)) {
        DROP_LOG(s, "malformed RTP datagram (%d bytes)", n);
        m_add(&s_metrics.drops_malformed, 1);
        return;
    }
    /* PT mapping note: the default PT 10 reuses the RFC 3551 L16 slot, but
     * this engine writes LITTLE-endian samples - a compliant RFC 3551 L16
     * sender would be byte-swapped, so PT 10 here is the project's LE
     * convention, NOT interoperable RFC 3551 L16. A dynamic PT (96-127)
     * avoids the association. */
    if (pkt.pt == CONFIG_MICHI_AUDIO_RTP_PT_S24LE) {
        /* Declared meta, NOT supported in phase 11: reject. */
        DROP_LOG(s, "RTP PT %u (S24LE) rejected: declared, not supported in phase 11",
                 (unsigned)pkt.pt);
        m_add(&s_metrics.drops_pt_s24le, 1);
        return;
    }
    if (pkt.pt != CONFIG_MICHI_AUDIO_RTP_PT_S16LE) {
        DROP_LOG(s, "RTP PT %u rejected (only %d = S16LE is accepted)",
                 (unsigned)pkt.pt, CONFIG_MICHI_AUDIO_RTP_PT_S16LE);
        m_add(&s_metrics.drops_pt_other, 1);
        return;
    }
    if (s->ssrc_filter != 0 && pkt.ssrc != s->ssrc_filter) {
        DROP_LOG(s, "RTP SSRC 0x%08" PRIx32 " filtered (session accepts 0x%08" PRIx32 ")",
                 pkt.ssrc, s->ssrc_filter);
        m_add(&s_metrics.drops_ssrc_filtered, 1);
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
     * arrival + (ts - base_ts) / sample_rate. uint64 accumulation (no wrap
     * of jitter_us*15 at ~4.8 min) and sample clamp (a sender stall is not
     * jitter; a pathological sample must not pin the estimate). */
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
        return 1; /* packet geometry unknown: any first packet seeds it */
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
 * deadline passes or the session is stopped. */
static void session_fill(session_t *s, uint32_t deadline_ms)
{
    const uint32_t start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    for (;;) {
        if (!s_session_run || s->jb.count >= prefill_target(s)) {
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

static void session_task(void *arg)
{
    session_t *s = (session_t *)arg;
    bool self_end = false; /* the task ended by itself, not via stop() */

    if (!s_session_run) {
        /* stop() raced with task creation: never open the port. */
        goto out;
    }

    s->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s->sock < 0) {
        ESP_LOGE(TAG, "session: socket() failed (errno %d)", errno);
        self_end = true;
        goto out;
    }
    struct timeval tv = {
        .tv_sec = 0,
        .tv_usec = MICHI_AUDIO_SESSION_SOCKET_TIMEOUT_MS * 1000,
    };
    setsockopt(s->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(s->port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s->sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "session: bind :%u failed (errno %d)", (unsigned)s->port, errno);
        self_end = true;
        goto out;
    }
    s_session_active = true;
    ESP_LOGI(TAG, "session: udp :%u ssrc=%s", (unsigned)s->port,
             s->ssrc_filter != 0 ? "filtered" : "first-seen");

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
        if (!session_drain(s)) {
            ESP_LOGE(TAG, "session: pipeline rejected a write - ending session");
            s_session_run = false;
            self_end = true;
            break;
        }
        if (s->jb.count == 0) {
            /* Underrun: explicit silence keeps the clocks running, then a
             * brief re-prefill resyncs the playhead to the next packet. */
            if (!s->in_underrun) {
                s->in_underrun = true;
                ESP_LOGW(TAG, "underrun: jitter buffer empty - explicit "
                              "silence + brief re-prefill");
                m_add(&s_metrics.underruns, 1);
            }
            if (!write_silence(s, s->samples_per_packet)) {
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
        /* The session ended BY ITSELF (pipeline write failure, socket/bind
         * failure): clear the active flag so session_start() can open a new
         * session. When stop() initiated the teardown it clears the flag
         * after the join; this local flag guarantees we never stomp a NEW
         * session's active bit (the clear happens strictly before the done
         * flag, which is what a new session_start() waits on). */
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
    ESP_LOGW(TAG, "prepare: %" PRIu32 "/%u/%u not supported (phase 11 supports "
                  "%d/%d/%d; other profiles are declared future metas)",
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
    if (CONFIG_MICHI_AUDIO_JITTER_MAX_MS < MICHI_AUDIO_PACKET_MS_ASSUMED) {
        ESP_LOGE(TAG, "init: MICHI_AUDIO_JITTER_MAX_MS=%d is below one packet",
                 CONFIG_MICHI_AUDIO_JITTER_MAX_MS);
        return ESP_ERR_INVALID_ARG;
    }
    if (CONFIG_MICHI_AUDIO_RTP_PT_S16LE == CONFIG_MICHI_AUDIO_RTP_PT_S24LE) {
        ESP_LOGE(TAG, "init: RTP payload types collide: PT %d cannot serve "
                      "both S16LE and S24LE",
                 CONFIG_MICHI_AUDIO_RTP_PT_S16LE);
        return ESP_ERR_INVALID_ARG;
    }
    if (CONFIG_MICHI_AUDIO_PREFILL_MS > CONFIG_MICHI_AUDIO_JITTER_MAX_MS) {
        ESP_LOGW(TAG, "init: prefill %d ms > jitter capacity %d ms - prefill "
                      "clamped at runtime",
                 CONFIG_MICHI_AUDIO_PREFILL_MS, CONFIG_MICHI_AUDIO_JITTER_MAX_MS);
    }
    ESP_LOGI(TAG, "init: pt_s16le=%d pt_s24le=%d jitter=%d ms (%d packets) "
                  "prefill=%d ms rx_buf=%d stack=%d",
             CONFIG_MICHI_AUDIO_RTP_PT_S16LE, CONFIG_MICHI_AUDIO_RTP_PT_S24LE,
             CONFIG_MICHI_AUDIO_JITTER_MAX_MS, MICHI_AUDIO_MAX_PACKETS,
             CONFIG_MICHI_AUDIO_PREFILL_MS, MICHI_AUDIO_RX_BUF_BYTES,
             CONFIG_MICHI_AUDIO_TASK_STACK_BYTES);
    s_initialized = true;
    return ESP_OK;
}

esp_err_t michi_audio_session_start(uint16_t port, uint32_t ssrc_filter)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_session_task != NULL) {
        if (!s_session_done) {
            ESP_LOGE(TAG, "session start: a session task already exists");
            return ESP_ERR_INVALID_STATE;
        }
        /* Stale handle: the previous task self-deleted (e.g. a bind
         * failure) before session_start assigned the handle. Clear it -
         * the task is gone (done flag observed). */
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
    portEXIT_CRITICAL(&s_lock);

    session_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s == NULL) {
        ESP_LOGE(TAG, "session start: session context allocation failed");
        return ESP_ERR_NO_MEM;
    }
    s->port = port;
    s->ssrc_filter = ssrc_filter;
    s->sock = -1;
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

    s_session_run = true;
    TaskHandle_t task = NULL;
    if (xTaskCreate(session_task, "michi_session", CONFIG_MICHI_AUDIO_TASK_STACK_BYTES,
                    s, MICHI_AUDIO_SESSION_TASK_PRIO, &task) != pdPASS) {
        ESP_LOGE(TAG, "session start: task creation failed");
        s_session_run = false;
        free(s->jb.entries);
        free(s->jb.pool);
        free(s->recv_buf);
        free(s->zeros);
        free(s);
        return ESP_ERR_NO_MEM;
    }
    s_session_task = task;
    ESP_LOGI(TAG, "session: task created (port %u, ssrc filter %s)",
             (unsigned)port, ssrc_filter != 0 ? "on" : "off");
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
    ESP_LOGI(TAG, "session: stopped");
    return ESP_OK;
}

bool michi_audio_session_active(void)
{
    return s_session_active;
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

    /* With BCLK/LRCK continuous the PCM5122 PLL can lock: re-run the phase-2
     * start for the validated profile. Honest on failure: the error is
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
