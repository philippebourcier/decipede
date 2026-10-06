// NTRIP caster (source) client on core 1, port of ntrip_caster.py.
// Streams the UM980 COM2 RTCM bytes to the caster, reconnecting forever.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char server[64];
    uint16_t port;
    char mountpoint[64];
    char user[64];
    char password[64];
} ntrip_params_t;

// Launch core 1. Params are copied.
void ntrip_start(const ntrip_params_t *params);
bool ntrip_started(void);
// Consecutive failed connection attempts (0 while connected).
uint32_t ntrip_failures(void);
// Close the caster session and hold streaming (e.g. during a UM980
// firmware upgrade). Waits until core 1 has let go of the socket.
bool ntrip_pause(uint32_t timeout_ms);
void ntrip_resume(void);
// Close the connection and stop streaming (before an OTA reboot).
void ntrip_stop(void);
