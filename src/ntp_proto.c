#include "ntp_proto.h"

#include <string.h>

uint64_t ntp_from_unix_us(int64_t unix_us) {
    int64_t s = unix_us / 1000000, us = unix_us % 1000000;
    if (us < 0) {
        s--;
        us += 1000000;
    }
    uint64_t frac = ((uint64_t)us << 32) / 1000000;
    return ((uint64_t)(s + NTP_UNIX_OFFSET) << 32) | (frac & 0xFFFFFFFFu);
}

int64_t ntp_to_unix_us(uint64_t ntp) {
    int64_t s = (int64_t)(ntp >> 32) - NTP_UNIX_OFFSET;
    int64_t us = (int64_t)(((ntp & 0xFFFFFFFFu) * 1000000) >> 32);
    return s * 1000000 + us;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}

bool ntp_build_reply(const uint8_t *req, size_t req_len, const ntp_times_t *t, uint8_t reply[NTP_PACKET_SIZE]) {
    if (req_len < NTP_PACKET_SIZE) return false;
    uint8_t vn = (req[0] >> 3) & 7, mode = req[0] & 7;
    if (mode != 3 || vn < 1 || vn > 4) return false;

    memset(reply, 0, NTP_PACKET_SIZE);
    uint8_t li = t->synced ? 0 : 3;
    reply[0] = (uint8_t)(li << 6 | vn << 3 | 4);  // mode 4: server
    reply[1] = t->synced ? 1 : 16;                  // stratum 1: GNSS reference clock
    reply[2] = req[2];                              // poll: echo the client's
    reply[3] = (uint8_t)(int8_t)-20;                // precision ~1 us
    put32(reply + 4, 0);                            // root delay
    put32(reply + 8, 0x00000010);                   // root dispersion ~0.25 ms
    memcpy(reply + 12, t->synced ? "PPS\0" : "INIT", 4);
    put64(reply + 16, t->synced ? ntp_from_unix_us(t->ref_unix_us) : 0);
    memcpy(reply + 24, req + 40, 8);                // origin = client's transmit
    put64(reply + 32, ntp_from_unix_us(t->rx_unix_us));
    put64(reply + 40, ntp_from_unix_us(t->tx_unix_us));
    return true;
}
