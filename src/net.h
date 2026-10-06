// W5500 Ethernet bring-up, DHCP/static IP, DNS. Port of network_init.py /
// wiznet_init.py on top of the WIZnet ioLibrary.
//
// Socket allocation (W5500 has 8):
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    SOCK_DHCP = 0,
    SOCK_DNS = 1,
    SOCK_NTRIP = 2,    // core 1
    SOCK_HTTPD = 3,    // status page, port 80
    SOCK_HTTPC = 4,    // config / telemetry / OTA client (core 0)
    SOCK_NTP = 5,      // NTP server, UDP 123
    SOCK_HTTPD2 = 6,   // second status page listener
    // 7: spare
};

typedef struct {
    bool link;
    bool has_ip;
    uint8_t mac[6];
    uint8_t ip[4], subnet[4], gateway[4], dns[4];
} net_info_t;

// SPI + W5500 reset + chip check (VERSIONR). Safe to call once at boot.
bool net_hw_init(void);
// Configure IP (DHCP or static) and wait up to timeout_s for link + address.
// Static mode needs ip/subnet/gateway/dns strings. Calls idle_poll().
bool net_start(bool dhcp, const char *ip, const char *subnet, const char *gateway,
               const char *dns, uint32_t timeout_s);
// Keep the DHCP lease alive; call regularly from core 0.
void net_poll(void);
bool net_is_up(void);
void net_get_info(net_info_t *info);
void net_print_status(void);

// Report a successful TCP connection to `peer` (any core). Only peers
// outside the local subnet count as Internet access; in static mode, 5 min
// without one switches to DHCP (see net.c).
void net_note_internet_ok(const uint8_t peer[4]);
// "DHCP", "static", "DHCP (static fallback)", ...
const char *net_mode(void);

// Resolve a hostname or dotted quad. Thread-safe (both cores).
bool net_resolve(const char *host, uint8_t ip[4]);

// Random local TCP port from the dynamic range (RFC 6056) using the
// hardware RNG, so a reboot never reuses the 4-tuple of a connection that
// died without a FIN (stale NAT/peer state). Safe from both cores.
uint16_t net_ephemeral_port(void);

// TCP segment size. The W5500 does no Path MTU Discovery: it sends with
// Don't-Fragment set and ignores ICMP "fragmentation needed", so a path
// with MTU < 1500 and no MSS clamping silently drops full-size segments.
// net_tcp_prepare() clears DF and sets the MSS announced in our SYN (which
// also caps what the peer sends us); net_tcp_mss_step_down() lowers it
// after an ACK stall (packetization-layer probing, RFC 4821 style).
void net_tcp_set_mss_limit(uint16_t mss);  // 0 = automatic (start at 1460)
uint16_t net_tcp_mss(void);
// Call between wz_socket() and wz_connect().
void net_tcp_prepare(uint8_t sn);
// Returns true if the MSS was lowered (false if already at the minimum).
bool net_tcp_mss_step_down(void);

// Non-blocking UDP send (ioLibrary's sendto blocks until ARP completes):
// start, then poll: 1 = sent, 0 = in progress, -1 = failed (ARP timeout).
bool net_udp_send_start(uint8_t sn, const uint8_t *buf, uint16_t len, const uint8_t ip[4], uint16_t port);
int net_udp_send_poll(uint8_t sn);

// Serialise W5500 access across cores (used by ioLibrary callbacks).
void net_lock(void);
void net_unlock(void);
