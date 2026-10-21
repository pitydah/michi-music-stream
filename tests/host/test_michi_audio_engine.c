/* H18: actual RTP engine integration test. Compiles production michi_audio.c,
 * michi_jb.c, rtp_guard.c and audio_buffer.c. ONLY I2S/DAC and FreeRTOS are
 * replaced; UDP runs on POSIX loopback. Never claim physical DAC coverage. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "michi_audio.h"
#include "freertos/task.h"
#include "rtp_output_fake.h"

static int failed = 0;
#define EXPECT(c, text) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", text, __LINE__); failed++; } } while(0)

static void send_packet(int fd, uint16_t port, uint16_t seq, uint32_t ssrc)
{
    uint8_t packet[12 + 1920] = {0};
    packet[0] = 0x80;
    packet[1] = 97;
    packet[2] = (uint8_t)(seq >> 8);
    packet[3] = (uint8_t)seq;
    const uint32_t ts = (uint32_t)seq * 480u;
    packet[4] = (uint8_t)(ts >> 24);
    packet[5] = (uint8_t)(ts >> 16);
    packet[6] = (uint8_t)(ts >> 8);
    packet[7] = (uint8_t)ts;
    packet[8] = (uint8_t)(ssrc >> 24);
    packet[9] = (uint8_t)(ssrc >> 16);
    packet[10] = (uint8_t)(ssrc >> 8);
    packet[11] = (uint8_t)ssrc;
    memset(packet + 12, 0x4a, 1920);
    const struct sockaddr_in dest = {
        .sin_family = AF_INET, .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };
    EXPECT(sendto(fd, packet, sizeof(packet), 0,
                  (const struct sockaddr *)&dest, sizeof(dest)) == (ssize_t)sizeof(packet),
           "send canonical RTP datagram");
}

static bool wait_received(uint32_t min_packets)
{
    for (int i = 0; i < 100; ++i) {
        michi_audio_metrics_t metrics = {0};
        if (michi_audio_get_metrics(&metrics) == ESP_OK && metrics.received >= min_packets)
            return true;
        usleep(10000);
    }
    return false;
}

int main(void)
{
    const uint32_t ssrc = 0x12345678u;
    test_rtp_output_reset();
    EXPECT(michi_audio_init() == ESP_OK, "init real RTP engine");
    test_rtp_heap_fail_next_allocation();
    EXPECT(michi_audio_session_start(0, ssrc, "127.0.0.1", 50) == ESP_ERR_NO_MEM,
           "real engine rejects context allocation failure without session");
    EXPECT(!michi_audio_session_active(), "allocation failure leaves no phantom session");
    test_rtp_bind_fail_next();
    EXPECT(michi_audio_session_start(49152, ssrc, "127.0.0.1", 50) != ESP_OK,
           "real engine closes failed bound socket before worker starts");
    EXPECT(!michi_audio_session_active(), "bind failure leaves no phantom session");
    test_task_set_create_fail(true);
    EXPECT(michi_audio_session_start(0, ssrc, "127.0.0.1", 50) == ESP_ERR_NO_MEM,
           "real engine rejects task creation failure and unwinds resources");
    test_task_set_create_fail(false);
    EXPECT(!michi_audio_session_active(), "task-create failure leaves no active engine");
    EXPECT(michi_audio_session_start(0, ssrc, "127.0.0.1", 50) == ESP_OK,
           "bind socket and create actual RTP worker");
    uint16_t port = 0;
    EXPECT(michi_audio_session_get_port(&port) == ESP_OK && port >= 49152,
           "real worker exposes receiver UDP port");
    int sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    EXPECT(sender >= 0, "UDP sender socket created");
    if (sender >= 0 && port != 0) {
        for (uint16_t i = 0; i < 12; i++) send_packet(sender, port, (uint16_t)(i + 1), ssrc);
        EXPECT(wait_received(5), "real RTP packet parser accepts negotiated SSRC");
        send_packet(sender, port, 13, 0x87654321u);
        usleep(120000);
        michi_audio_metrics_t metrics = {0};
        EXPECT(michi_audio_get_metrics(&metrics) == ESP_OK, "read real engine metrics");
        EXPECT(metrics.drops_ssrc_filtered >= 1, "real guard rejects wrong SSRC");
        EXPECT(test_rtp_output_written_bytes() > 0, "real worker submits PCM to fake I2S");
        /* H19: ring admission is counted only for accepted RTP PCM, never
         * for explicit gap/underrun silence or for rejected datagrams. */
        EXPECT(metrics.pcm_packets_submitted > 0,
               "actual RTP PCM accepted into I2S ring");
        EXPECT(metrics.pcm_bytes_submitted ==
               (uint64_t)metrics.pcm_packets_submitted * 1920u,
               "PCM ring byte accounting excludes non-RTP silence");
        EXPECT(metrics.last_pcm_submit_monotonic_us > 0,
               "last accepted PCM admission has a monotonic timestamp");

        /* PAUSE occurs while recvfrom may be blocked. No stale PCM admission
         * may terminate the real worker after the QUIESCE acknowledgement. */
        EXPECT(michi_audio_session_set_paused(true) == ESP_OK, "pause real worker");
        for (uint16_t i = 0; i < 12; i++) send_packet(sender, port, (uint16_t)(i + 14), ssrc);
        usleep(150000);
        EXPECT(michi_audio_session_active(), "blocked recvfrom + pause preserves session");
        EXPECT(michi_audio_session_set_paused(false) == ESP_OK, "resume real worker");
        for (uint16_t i = 0; i < 12; i++) send_packet(sender, port, (uint16_t)(i + 30), ssrc);
        EXPECT(wait_received(10), "worker receives more RTP after resume");
    }
    if (sender >= 0) close(sender);
    EXPECT(michi_audio_session_stop() == ESP_OK, "stop/join real RTP worker");
    EXPECT(!michi_audio_session_active(), "no phantom session after stop");
    EXPECT(michi_audio_session_start(0, ssrc, "127.0.0.1", 50) == ESP_OK,
           "new real RTP session starts after stop");
    michi_audio_metrics_t new_session_metrics = {0};
    EXPECT(michi_audio_get_metrics(&new_session_metrics) == ESP_OK,
           "new session metrics accessible");
    EXPECT(new_session_metrics.pcm_packets_submitted == 0 &&
           new_session_metrics.pcm_bytes_submitted == 0 &&
           new_session_metrics.last_pcm_submit_monotonic_us == 0,
           "new RTP session resets prior PCM admission evidence");
    test_rtp_output_force_quiesce_fail(true);
    EXPECT(michi_audio_session_stop() == ESP_FAIL,
           "real session propagates QUIESCE failure after transport closes");
    EXPECT(!michi_audio_session_active(), "failed silence never retains a ghost RTP session");
    test_rtp_output_force_quiesce_fail(false);
    EXPECT(michi_audio_session_start(0, ssrc, "127.0.0.1", 50) == ESP_OK,
           "faulted output reconciles on next real session start");
    EXPECT(michi_audio_session_stop() == ESP_OK, "recovered real RTP worker stops");
    EXPECT(test_task_external_delete_count() == 0, "no external FreeRTOS task destruction");
    if (failed) fprintf(stderr, "real RTP host integration: %d failed assertions\n", failed);
    return failed ? 1 : 0;
}
