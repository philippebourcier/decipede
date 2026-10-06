// NTPv3/v4 server packet handling (pure, host-tested).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NTP_PACKET_SIZE 48
#define NTP_UNIX_OFFSET 2208988800LL  // seconds from 1900-01-01 to 1970-01-01

// 64-bit NTP timestamp (32.32 fixed point, seconds since 1900) from unix us.
uint64_t ntp_from_unix_us(int64_t unix_us);
int64_t ntp_to_unix_us(uint64_t ntp);

typedef struct {
    bool synced;           // false: LI=3 (alarm), stratum 16
    int64_t ref_unix_us;   // last time the clock was set (reference timestamp)
    int64_t rx_unix_us;    // request arrival
    int64_t tx_unix_us;    // reply transmission
} ntp_times_t;

// Build the reply to a client request (mode 3). Returns false (no reply)
// for anything that isn't a well-formed client request.
bool ntp_build_reply(const uint8_t *req, size_t req_len, const ntp_times_t *t, uint8_t reply[NTP_PACKET_SIZE]);
