# Networking

## W5500 sockets

The W5500 has 8 hardware sockets, sharing 16 KB of TX buffer and 16 KB of RX
buffer.

| # | Use | TX/RX KB |
|---|---|---|
| 0 | DHCP | 1/1 |
| 1 | DNS | 1/1 |
| 2 | NTRIP (core 1) | 8/1 |
| 3 | Web server #1 | 2/2 |
| 4 | HTTP client (config, telemetry, updates) | 1/8 |
| 5 | NTP server | 1/1 |
| 6 | Web server #2 | 2/2 |
| 7 | Spare | 0/0 |

**Web server:**
- Two listeners on port 80, so two clients are served at once. The W5500
  has no listen backlog, so a third simultaneous connection is refused.
- A listener stuck in a closing state (client gone) is force-closed after
  2 s.
- The state of all 8 sockets is in `/status.json` (`sockets`).

## NTRIP robustness

- **NTRIP runs on core 1,** independently of the web server, the sensors
  and the updates on core 0.
- **Retries:** the connection is retried forever. RTCM that piled up while
  disconnected is dropped, so the caster never gets stale corrections.
- **Random local ports:** every outgoing TCP connection uses a port from
  49152–65535, chosen by the hardware RNG. A reboot without a clean close
  never reuses the 4-tuple of a dead session, which stale NAT or peer state
  would otherwise swallow.
- **No-ACK detection:** if the caster acknowledges nothing for 10 s while
  data is queued, the session is reconnected on a fresh port. The reason is
  logged (`NTRIP stream ended: …`).
- **Path MTU:**
  - The W5500 does no PMTU discovery: it sends with Don't-Fragment set and
    ignores ICMP "fragmentation needed".
  - The firmware clears DF on its TCP sockets, so IPv4 routers fragment
    instead of dropping.
  - After an ACK stall with full-size segments in flight, the session
    reconnects with a smaller MSS: 1460 → 1360 → 1200 → 536. The current
    value is `tcp_mss` in `/status.json`.
  - `tcp_mss` in `config.json` pins the MSS for a known link.

## Static IP fallback

With `"dhcp": false`, the station uses `ip`, `subnet`, `gateway` and `dns`.

- **Switch to DHCP:** if no connection to a host *outside the local subnet*
  succeeds for 5 min, the station switches to DHCP.
  - Successes are config, telemetry or update requests, or the NTRIP
    caster.
  - LAN-only successes don't count, because a wrong gateway leaves the LAN
    reachable.
- **Back to static:** if no lease arrives within 2 min, the station returns
  to the static settings, and only tries DHCP again 30 min later.
- **The stored config isn't changed:** the next boot starts with the static
  settings again.
- **Invalid static settings** go to DHCP straight away.
- `net_mode` in `/status.json` shows the current mode.

## NTP server

`"ntp_server": true` in `config.json` turns on a stratum-1 NTP server on UDP
123, with refid `PPS`.

- **Receive timestamps** come from the W5500 interrupt line (GP21, falling
  edge on the NTP socket's receive event). They are accurate to
  microseconds, even though the socket is serviced only every ~20 ms.
- **Uncertain arrival time:** a request whose arrival time can't be known
  exactly (queued behind other packets) is dropped, not answered with a
  wrong time. Drops are counted in `ntp_dropped`.
- **No valid UTC clock** (no PPS): the server answers with LI=3 / stratum 16,
  so clients ignore it.

## Behaviour on noisy or hostile LANs

- **The W5500 filters most chatter in hardware:**
  - The firmware never joins a multicast group, so mDNS, SSDP and LLMNR are
    dropped.
  - The NTP and DNS sockets block broadcasts. Only DHCP listens to
    broadcasts, on its own port.
  - ARP and ping are answered by the chip.
- **UDP sends never block:**
  - ioLibrary's `sendto()` spins until ARP completes.
  - DNS and NTP use a non-blocking send instead, and DNS is a built-in
    non-blocking client.
  - So a dead DNS server or a vanished NTP client can't stall a core or
    trigger the watchdog.
- **Measured: broadcast flood.** 28,000 mDNS and broadcast packets in 60 s
  had no effect: status page 12/12, NTP 12/12 at +0.08 ms, NTRIP
  unaffected.
- **Measured: targeted junk.** About 1,600 packets/s were aimed at the board:
  malformed UDP (including to port 123), TCP garbage, and silent connections
  on port 80.
  - No reset, and NTRIP was unaffected (0 overruns).
  - NTP kept answering (29/30), within a few ms.
  - Garbage requests get a 2-line `400`. Silent connections are dropped
    after 2 s.
  - The status page is mostly unavailable while someone deliberately keeps
    both listeners busy.
