/*
 * SENDER — Dual-layer FEC reliable real-time media transport.
 *
 * Strategy:
 *   Layer 1 (K=2): XOR parity over every pair of consecutive frames.
 *                  Fast recovery — FEC arrives 20ms after first frame.
 *   Layer 2 (K=4): XOR parity over every quad of consecutive frames.
 *                  Backup — catches cases where Layer 1 FEC is lost.
 *
 * Overhead budget (no feedback):
 *   Data:    1500 × 165 = 247,500 B
 *   K2 FEC:  750 × 166  = 124,500 B
 *   K4 FEC:  375 × 166  =  62,250 B
 *   Total:  434,250 / 240,000 = 1.81× (cap 2.0×)  ✓
 *
 * Wire format (sender -> relay -> receiver):
 *   DATA:  [0x01][4B BE seq][160B payload]                   = 165 bytes
 *   FEC:   [0x02][4B BE base_seq][1B count][160B XOR parity] = 166 bytes
 *          count=2 → K=2 pair FEC;  count=4 → K=4 quad FEC
 *
 * Ports (all 127.0.0.1):
 *   bind 47010  <- harness source delivers frame i at t0 + i*20ms
 *   send 47001  -> relay uplink (media + FEC)
 */

#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PAYLOAD_LEN  160
#define MAX_FRAMES  8000

#define PKT_DATA  0x01
#define PKT_FEC   0x02

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void) {
    const char *t0_s = getenv("T0");
    const char *dur_s = getenv("DURATION_S");
    if (!t0_s || !dur_s) {
        fprintf(stderr, "sender: T0 / DURATION_S not set\n");
        return 1;
    }
    double t0       = atof(t0_s);
    double duration = atof(dur_s);
    double end_time = t0 + duration + 2.0;

    /* ── Socket: receive from harness source (port 47010) ──────────── */
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family      = AF_INET;
    in_addr.sin_port        = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr)) < 0) {
        perror("bind 47010"); return 1;
    }

    /* ── Socket: send to relay (port 47001) ────────────────────────── */
    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay = {0};
    relay.sin_family      = AF_INET;
    relay.sin_port        = htons(47001);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* ── FEC accumulators ──────────────────────────────────────────── */
    unsigned char fec2_buf[PAYLOAD_LEN];   /* K=2 running XOR  */
    int fec2_count = 0;
    unsigned char fec4_buf[PAYLOAD_LEN];   /* K=4 running XOR  */
    int fec4_count = 0;

    memset(fec2_buf, 0, PAYLOAD_LEN);
    memset(fec4_buf, 0, PAYLOAD_LEN);

    /* ── Event loop ────────────────────────────────────────────────── */
    struct pollfd pfd = { .fd = in_fd, .events = POLLIN };
    unsigned char pkt[2048];

    while (now_sec() < end_time) {
        int ret = poll(&pfd, 1, 10);
        if (ret <= 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        ssize_t n = recvfrom(in_fd, pkt, sizeof(pkt), 0, NULL, NULL);
        if (n != 4 + PAYLOAD_LEN) continue;

        uint32_t seq;
        memcpy(&seq, pkt, 4);
        seq = ntohl(seq);

        /* ── Send DATA packet: [type][seq][payload] ────────────────── */
        unsigned char data_out[1 + 4 + PAYLOAD_LEN];
        data_out[0] = PKT_DATA;
        memcpy(data_out + 1, pkt, 4);              /* BE seq   */
        memcpy(data_out + 5, pkt + 4, PAYLOAD_LEN);
        sendto(out_fd, data_out, sizeof(data_out), 0,
               (struct sockaddr *)&relay, sizeof(relay));

        /* ── Layer 1: accumulate K=2 pair FEC ──────────────────────── */
        for (int j = 0; j < PAYLOAD_LEN; j++)
            fec2_buf[j] ^= pkt[4 + j];
        fec2_count++;

        if (fec2_count == 2) {
            uint32_t base = seq - 1;
            uint32_t be_base = htonl(base);
            unsigned char fec_out[1 + 4 + 1 + PAYLOAD_LEN];
            fec_out[0] = PKT_FEC;
            memcpy(fec_out + 1, &be_base, 4);
            fec_out[5] = 2;
            memcpy(fec_out + 6, fec2_buf, PAYLOAD_LEN);
            sendto(out_fd, fec_out, sizeof(fec_out), 0,
                   (struct sockaddr *)&relay, sizeof(relay));
            memset(fec2_buf, 0, PAYLOAD_LEN);
            fec2_count = 0;
        }

        /* ── Layer 2: accumulate K=4 quad FEC ──────────────────────── */
        for (int j = 0; j < PAYLOAD_LEN; j++)
            fec4_buf[j] ^= pkt[4 + j];
        fec4_count++;

        if (fec4_count == 4) {
            uint32_t base = seq - 3;
            uint32_t be_base = htonl(base);
            unsigned char fec_out[1 + 4 + 1 + PAYLOAD_LEN];
            fec_out[0] = PKT_FEC;
            memcpy(fec_out + 1, &be_base, 4);
            fec_out[5] = 4;
            memcpy(fec_out + 6, fec4_buf, PAYLOAD_LEN);
            sendto(out_fd, fec_out, sizeof(fec_out), 0,
                   (struct sockaddr *)&relay, sizeof(relay));
            memset(fec4_buf, 0, PAYLOAD_LEN);
            fec4_count = 0;
        }
    }
    return 0;
}
