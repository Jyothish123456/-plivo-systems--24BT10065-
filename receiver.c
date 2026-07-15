/*
 * RECEIVER — Dual-layer FEC recovery + immediate forwarding.
 *
 * Recovery layers:
 *   1. Direct: forward every DATA packet to player immediately on receipt.
 *   2. K=2 pair FEC: if 1 frame missing in pair and FEC present → XOR recover.
 *   3. K=4 quad FEC: if 1 frame missing in quad and FEC present → XOR recover.
 *      Catches cases where the K=2 FEC packet was itself dropped by the relay.
 *
 * Wire format (relay -> receiver):
 *   DATA:  [0x01][4B BE seq][160B payload]                   = 165 bytes
 *   FEC:   [0x02][4B BE base_seq][1B count][160B XOR parity] = 166 bytes
 *          count=2 → K=2 pair FEC;  count=4 → K=4 quad FEC
 *
 * Ports (all 127.0.0.1):
 *   bind 47002  <- media from sender via relay
 *   send 47020  -> harness player (4B BE seq + 160B payload)
 */

#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PAYLOAD_LEN    160
#define MAX_FRAMES    8000
#define MAX_K2_GROUPS (MAX_FRAMES / 2 + 1)
#define MAX_K4_GROUPS (MAX_FRAMES / 4 + 1)

#define PKT_DATA  0x01
#define PKT_FEC   0x02

/* ── Per-frame state ────────────────────────────────────────────────── */
static unsigned char payloads[MAX_FRAMES][PAYLOAD_LEN];
static int received[MAX_FRAMES];    /* have data for this seq       */
static int delivered[MAX_FRAMES];   /* forwarded to player          */

/* ── FEC state ──────────────────────────────────────────────────────── */
static unsigned char fec2_parity[MAX_K2_GROUPS][PAYLOAD_LEN];
static int fec2_ok[MAX_K2_GROUPS];

static unsigned char fec4_parity[MAX_K4_GROUPS][PAYLOAD_LEN];
static int fec4_ok[MAX_K4_GROUPS];

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Deliver frame to harness player */
static void deliver(int out_fd, const struct sockaddr_in *player,
                    uint32_t seq, const unsigned char *payload) {
    unsigned char buf[4 + PAYLOAD_LEN];
    uint32_t be_seq = htonl(seq);
    memcpy(buf, &be_seq, 4);
    memcpy(buf + 4, payload, PAYLOAD_LEN);
    sendto(out_fd, buf, sizeof(buf), 0,
           (const struct sockaddr *)player, sizeof(*player));
}

/* Try K=2 pair FEC recovery. Returns 1 if a frame was recovered. */
static int try_fec2(int group, int n_frames,
                    int out_fd, const struct sockaddr_in *player) {
    if (!fec2_ok[group]) return 0;
    int base = group * 2;
    int end  = base + 2;
    if (end > n_frames) end = n_frames;

    int missing = -1, miss_count = 0;
    for (int i = base; i < end; i++) {
        if (!received[i]) { missing = i; miss_count++; }
    }
    if (miss_count != 1 || delivered[missing]) return 0;

    /* XOR parity with all received payloads → missing payload */
    unsigned char recovered[PAYLOAD_LEN];
    memcpy(recovered, fec2_parity[group], PAYLOAD_LEN);
    for (int i = base; i < end; i++) {
        if (i != missing)
            for (int j = 0; j < PAYLOAD_LEN; j++)
                recovered[j] ^= payloads[i][j];
    }

    memcpy(payloads[missing], recovered, PAYLOAD_LEN);
    received[missing]  = 1;
    delivered[missing] = 1;
    deliver(out_fd, player, (uint32_t)missing, recovered);
    return 1;
}

/* Try K=4 quad FEC recovery. Returns 1 if a frame was recovered. */
static int try_fec4(int group, int n_frames,
                    int out_fd, const struct sockaddr_in *player) {
    if (!fec4_ok[group]) return 0;
    int base = group * 4;
    int end  = base + 4;
    if (end > n_frames) end = n_frames;

    int missing = -1, miss_count = 0;
    for (int i = base; i < end; i++) {
        if (!received[i]) { missing = i; miss_count++; }
    }
    if (miss_count != 1 || delivered[missing]) return 0;

    unsigned char recovered[PAYLOAD_LEN];
    memcpy(recovered, fec4_parity[group], PAYLOAD_LEN);
    for (int i = base; i < end; i++) {
        if (i != missing)
            for (int j = 0; j < PAYLOAD_LEN; j++)
                recovered[j] ^= payloads[i][j];
    }

    memcpy(payloads[missing], recovered, PAYLOAD_LEN);
    received[missing]  = 1;
    delivered[missing] = 1;
    deliver(out_fd, player, (uint32_t)missing, recovered);
    return 1;
}

int main(void) {
    const char *t0_s    = getenv("T0");
    const char *dur_s   = getenv("DURATION_S");
    const char *delay_s = getenv("DELAY_MS");
    if (!t0_s || !dur_s || !delay_s) {
        fprintf(stderr, "receiver: T0/DURATION_S/DELAY_MS not set\n");
        return 1;
    }
    double t0       = atof(t0_s);
    double duration = atof(dur_s);
    double delay_ms = atof(delay_s);
    int n_frames    = (int)(duration * 1000.0 / 20.0);
    double end_time = t0 + delay_ms / 1000.0
                    + (n_frames - 1) * 0.020 + 1.0;

    memset(received,  0, sizeof(received));
    memset(delivered, 0, sizeof(delivered));
    memset(fec2_ok,   0, sizeof(fec2_ok));
    memset(fec4_ok,   0, sizeof(fec4_ok));

    /* ── Socket: receive from relay (port 47002) ───────────────────── */
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family      = AF_INET;
    in_addr.sin_port        = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr)) < 0) {
        perror("bind 47002"); return 1;
    }

    /* ── Socket: send to harness player (port 47020) ───────────────── */
    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in player = {0};
    player.sin_family      = AF_INET;
    player.sin_port        = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* ── Main event loop ───────────────────────────────────────────── */
    struct pollfd pfd = { .fd = in_fd, .events = POLLIN };
    unsigned char pkt[2048];

    while (now_sec() < end_time) {
        int ret = poll(&pfd, 1, 5);
        if (ret <= 0 || !(pfd.revents & POLLIN)) continue;

        ssize_t n = recvfrom(in_fd, pkt, sizeof(pkt), 0, NULL, NULL);
        if (n < 1) continue;

        /* ── DATA packet ───────────────────────────────────────────── */
        if (pkt[0] == PKT_DATA && n >= 1 + 4 + PAYLOAD_LEN) {
            uint32_t seq;
            memcpy(&seq, pkt + 1, 4);
            seq = ntohl(seq);

            if (seq < (uint32_t)n_frames && !delivered[seq]) {
                /* Store for FEC recovery */
                memcpy(payloads[seq], pkt + 5, PAYLOAD_LEN);
                received[seq]  = 1;

                /* Forward immediately to player */
                delivered[seq] = 1;
                deliver(out_fd, &player, seq, pkt + 5);

                /* New data may complete an FEC group */
                try_fec2(seq / 2, n_frames, out_fd, &player);
                try_fec4(seq / 4, n_frames, out_fd, &player);
            }
        }
        /* ── FEC packet ────────────────────────────────────────────── */
        else if (pkt[0] == PKT_FEC && n >= 1 + 4 + 1 + PAYLOAD_LEN) {
            uint32_t base_seq;
            memcpy(&base_seq, pkt + 1, 4);
            base_seq = ntohl(base_seq);
            int count = pkt[5];

            if (count == 2) {
                int group = (int)base_seq / 2;
                if (group >= 0 && group < MAX_K2_GROUPS && !fec2_ok[group]) {
                    fec2_ok[group] = 1;
                    memcpy(fec2_parity[group], pkt + 6, PAYLOAD_LEN);
                    try_fec2(group, n_frames, out_fd, &player);
                }
            } else if (count == 4) {
                int group = (int)base_seq / 4;
                if (group >= 0 && group < MAX_K4_GROUPS && !fec4_ok[group]) {
                    fec4_ok[group] = 1;
                    memcpy(fec4_parity[group], pkt + 6, PAYLOAD_LEN);
                    try_fec4(group, n_frames, out_fd, &player);
                }
            }
        }
    }
    return 0;
}
