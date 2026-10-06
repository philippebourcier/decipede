// Stratum-1 NTP server on UDP 123 (one W5500 socket), fed by the PPS-
// disciplined UTC clock. Enabled by the remote config flag "ntp_server".
// Requests are timestamped by the W5500 interrupt line (GPIO21), so the
// receive timestamp is accurate to microseconds even though the socket is
// only serviced every ~20 ms.
#pragma once

#include <stdint.h>

void ntp_server_init(void);
// Open/close the socket per config and answer pending requests (core 0).
void ntp_server_poll(void);
uint32_t ntp_server_requests(void);
// Packets dropped because their arrival time couldn't be known (backlog).
uint32_t ntp_server_dropped(void);
