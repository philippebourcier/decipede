// UM980 auxiliary signals: PPS capture, RTK_STAT, reset line.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t count;          // rising edges since boot
    uint64_t last_edge_us;   // time_us_64() of the last edge (0 = none)
    uint32_t last_period_us; // interval between the last two edges
    int32_t dev_min_us;      // min/max deviation from 1 s since the last
    int32_t dev_max_us;      //   pps_reset_stats()
} pps_info_t;

// Configure pins (all inputs; the reset line is only ever pulled low) and
// the PPS interrupt. Checks that RESET reads high (pulled up by the UM980).
void gnss_io_init(void);

void pps_get(pps_info_t *out);
void pps_reset_stats(void);
// An edge arrived within the last 1.5 s.
bool pps_fresh(void);
// Wait (polling idle work) for the next edge; returns its timestamp or 0.
uint64_t pps_wait_edge(uint32_t timeout_ms);

bool gnss_rtk_stat(void);

// False if the reset line didn't read high at boot (wiring not as expected):
// all reset-based features are then disabled.
bool gnss_reset_available(void);
// Pull RESET low for ms, then release it (high-Z). Returns false if the
// reset line is unavailable.
bool gnss_reset_pulse(uint32_t ms);
