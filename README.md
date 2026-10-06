# decipede: RP2350 RTK base station firmware

C/Pico SDK port of [uPyRTKBase](https://github.com/philippebourcier/uPyRTKBase)
for the **WIZnet W5500-EVB-Pico2** + **Unicore UM980**. It has the same boot
sequence, LED codes, status page, telemetry and config server protocol as the
MicroPython version, and adds **A/B over-the-air updates with automatic
rollback** using the RP2350 bootrom.

**Hardware:** wiring, GPIO map and parts list in [HARDWARE.md](HARDWARE.md).

## Layout

```
build.sh        one-step build (fetches dependencies on first run)
HARDWARE.md     parts, wiring and GPIO map
src/            firmware sources (one module per Python file, plus ota.c, flash_store.c, http_*.c)
boards/         board header (W5500-EVB-Pico2, 16 MiB flash)
partitions.json flash partition table (App A / App B / Config)
VERSION         firmware version, MAJOR.MINOR.PATCH (bump for every release)
provision/      defaults.env (branding, tracked) + device.env (CONFIG_URL, git-ignored)
web/            status dashboard (index.html, logo.svg), embedded gzipped
tools/          setup.sh (dependencies), build_web.py, gen_ca_bundle.py (used by the build)
third_party/    jsmn JSON parser, TLS root certificates
lib/            git submodules: pico-sdk 2.3.1, WIZnet ioLibrary_Driver, picotool 2.3.1
```

| Python                      | C                                   |
|-----------------------------|-------------------------------------|
| base.py / main.py           | main.c, http_server.c, app_status.c |
| config_manager.py, .env     | config.c, flash_store.c, http_client.c |
| network_init.py, wiznet_init.py | net.c (ioLibrary, DHCP, DNS)    |
| ntrip_caster.py             | ntrip.c (core 1)                    |
| um980_config.py             | um980.c                             |
| rtcm_decoder.py, rtcm_params.py | rtcm.c                          |
| lsm6dsv.py, sht4x.py        | lsm6dsv.c, sht4x.c                  |
| rgb_led_drv.py              | led.c                               |
| wdt.py                      | wdt.c, idle.c                       |
| rtkbase.py (unused legacy)  | not ported                          |

## Build

```sh
git clone https://github.com/philippebourcier/decipede.git
cd decipede
cp provision/device.env.example provision/device.env   # set CONFIG_URL (see "Provisioning")
./build.sh                                             # -> build/rtkbase.uf2
```

- **Needs:** git, cmake ≥ 3.13, make, a host C/C++ compiler (gcc/g++ or
  clang), python3, curl, tar and xz. No root and no other packages.
- **First run** (`tools/setup.sh`, called by `build.sh`):
  - initialises the `lib/` submodules: pico-sdk with only its tinyusb and
    mbedtls submodules, ioLibrary_Driver, picotool;
  - downloads the Arm GNU toolchain 14.3 into `.deps/` and checks it against
    Arm's published SHA-256;
  - builds picotool into `.deps/`. The SDK's prebuilt picotool lacks `seal`,
    which the image hashing needs.
- **Later runs** only rebuild what changed.
- **Options:**
  - `JOBS=3 ./build.sh` limits parallel jobs (default: all CPUs).
  - `PICO_TOOLCHAIN_PATH=/path/to/arm-gnu-toolchain ./build.sh` uses an
    existing toolchain instead of downloading one.
- **Platforms:** the toolchain download covers Linux x86_64/aarch64 and macOS
  arm64/x86_64. Only Linux x86_64 has been tested.
- **Libraries:** all are used unmodified, at the commits pinned by the
  submodules. Their known bugs are worked around in `src/` (e.g. `wiz.h`).

Outputs: `build/rtkbase.uf2` (application) and `build/partitions.uf2` (partition table).

## Provisioning: config URL and branding

**The config.json URL is the only URL built into the firmware.** Everything
else (NTRIP caster and credentials, telemetry, RP2350 OTA and UM980 firmware
manifests, maintenance window, …) comes from that config.json.

Build-time settings come from two `KEY=value` files:

| File | Tracked | Purpose |
|---|---|---|
| `provision/defaults.env` | yes | The project's defaults: Centipede-RTK branding and logo. **A network forking this firmware edits this file.** |
| `provision/device.env` | no (git-ignored) | One deployment's values. Same keys; overrides `defaults.env`. Start from `device.env.example`. |

| Key | Used for |
|---|---|
| `CONFIG_URL` | the config server; the device appends `?b=<hardware id>`. It can't be changed by the remote config, so a bad value can't cut a device off. |
| `BRAND_NAME`, `BRAND_TAGLINE` | dashboard title, subtitle and footer; `/basic` title |
| `BRAND_LOGO` | dashboard logo and favicon: an SVG, path relative to the repository |

config.json keys for the rest:

| Key | Used for | Without it |
|---|---|---|
| `ntrip_server`, `ntrip_port`, `ntrip_mountpoint`, `ntrip_user`, `ntrip_password` | the caster | no NTRIP (port defaults to 2101) |
| `telemetry_url` | telemetry POST every 5 min | no telemetry |
| `firmware_url` | the firmware manifest for both chips (see "Firmware updates") | no updates; the update button is hidden |
| `maint_window` | UTC window for UM980 flashing, `"HH:MM-HH:MM"` | scheduled UM980 upgrades never flash (the button still can) |

- Every URL may be `http://` or `https://`. For HTTPS from a private CA, add
  the CA to `provision/ca.pem` (see "HTTPS").
- The device stores the config it receives. A key missing from a later
  config.json keeps its stored value.
- The build fails on unknown keys, on values containing quotes,
  backslashes or `< >`, and on `%` in the `BRAND_*` values. The former URL
  keys (`TELEMETRY_URL`, `OTA_URL`, …) fail with a message naming their
  config.json key.
- Changing branding rebuilds the dashboard. Keep the logo small (a few KB):
  it's embedded in the firmware and the page must stay under 20 KB gzipped.
- To build for another deployment without touching your files:
  `cmake -B build-other -DPROVISION_FILE=/path/to/other.env`.

## Flash layout

| Region   | Offset    | Size    | Content                                   |
|----------|-----------|---------|-------------------------------------------|
| PT       | 0x000000  | 8 KB    | partition table                           |
| App A    | 0x002000  | 2040 KB | application slot                          |
| App B    | 0x200000  | 2048 KB | application slot (linked to A)            |
| Config   | 0x400000  | 64 KB   | config JSON + OTA state (2 × 2 sectors, power-cut safe) |
| (free)   | 0x410000  | ~12 MB  | unused                                    |

## First install (replaces MicroPython)

1. Put the board in BOOTSEL mode: hold BOOTSEL while plugging in USB, or from
   MicroPython run `mpremote exec "import machine; machine.bootloader()"`.
   An `RP2350` USB drive appears.
2. Copy `build/partitions.uf2` to the drive. The board reboots back into BOOTSEL
   (no application installed yet).
3. Copy `build/rtkbase.uf2` to the drive. The board reboots into the firmware.

The first boot fetches config.json from the provisioned `CONFIG_URL` (it
provides the caster, credentials and every other URL) and stores it in the
Config partition. The Ethernet MAC is a locally administered address hashed from the full
64-bit RP2350 chip ID (splitmix64, 46 effective bits, since 1.3.0). Each
board has a stable MAC; for 100,000 boards the chance of any duplicate is
~0.007 %. A factory log of `hardware_id`/MAC catches the rest.

**Back to MicroPython:** in BOOTSEL mode, erase the flash (`picotool erase`) and
copy a MicroPython UF2, or a full-flash backup taken before installing this firmware.

## Firmware updates (RP2350 and UM980)

One manifest describes both firmwares. Set its URL as `firmware_url` in
config.json:

```json
{"rp2350": {"version": "1.7.0", "url": "https://server/fw/rtkbase-1.7.0.uf2", "size": 668160, "sha256": "…"},
 "um980":  {"model": "UM980", "version": "R4.10Build20739", "url": "https://server/fw/UM980_R4.10Build20739.pkg",
            "size": 3038408, "sha256": "…"}}
```

- **Sections are optional:** either one may be absent when nothing is
  published for that chip.
- **The request** carries `?b=<hardware id>&fw=<RP2350 version>&um980=<UM980 version>`,
  so the server can stage rollouts.
- **Caching:** the file is downloaded once per check and cached for 60 s, so
  the UM980 and RP2350 checks share one download. The button always fetches
  it fresh.
- **Schedule:** the RP2350 part is checked ~60 s after boot, the UM980 part
  ~3 min after boot. Both are then checked every `ota_interval_h` hours
  (default 6) and on the button.

Release procedure:
1. Bump `VERSION`, then run `./build.sh`.
2. Put `build/rtkbase.uf2` (renamed, e.g. `rtkbase-1.7.0.uf2`) and/or the
   UM980 `.pkg` on your server.
3. Update the manifest entries. `size` is the file size in bytes; `sha256` is
   the lowercase hex SHA-256 of the file (`sha256sum`). For UM980 packages,
   `version` must contain the build (`R4.10BuildNNNNN`). Packages must be
   genuine UM980 `.pkg` files of 4 MiB or less.

### RP2350 (board firmware)

What happens on the device:

1. **Download:** if the version is newer, the UF2 is streamed into the
   *inactive* slot and SHA-256 is computed on the fly. A size, sequence or hash
   mismatch erases the partial image. LED1 turns purple while downloading.
   RTCM streaming continues (the UM980 data is DMA-buffered).
2. **Trial boot:** the board reboots into the new slot as a
   *try-before-you-buy* image. Within the bootrom's ~16.7 s window it must
   load its config and detect the W5500, then it calls `rom_explicit_buy`.
   If it crashes or hangs first, the bootrom boots the previous image.
3. **Health check:** the bought image must then reach the network and
   either download its config or connect to the caster within 10 minutes.
   Otherwise it erases its own image header and reboots into the previous image.
4. **Rollback bookkeeping:** the previous image notices the failed version,
   shows "rolled back from X" on the status page, and stops retrying a
   version after 2 failures.

Images carry an embedded SHA-256 hash (checked by the bootrom) and a version
number (`VERSION` → MAJOR / MINOR·256+PATCH), which the bootrom uses to pick
the newer valid slot.

**Security note:** use `https://` URLs (see HTTPS below) so manifests and
images can't be swapped in transit. The SHA-256 in the manifest protects
against corruption, not against someone who controls the server. For authenticated
updates, sign images (`pico_sign_binary`) and enable secure boot in OTP. That
is **irreversible** and needs a key-management plan first.

## LED2 (GNSS / antenna)

Same codes as the Python version (orange booting, yellow UM980 init, cyan checks
pending, green good, blue AGC degraded, pink vibrating/tilted, red UM980 failure),
plus:

- **Blinking red (1 Hz): no PPS.** The receiver answers but has had no satellite
  lock for 60 s (antenna disconnected or no sky view), so no corrections are
  produced. It overrides green, blue and pink, but not solid red (receiver
  failure). It clears by itself when PPS returns. `led2` in `/status.json`
  reads `RED_BLINK`.

## Behaviour differences from the Python version

- NTRIP retries forever (the Python thread gave up after 120 / 3600 attempts)
  and drops RTCM that piled up while disconnected instead of sending stale
  corrections.
- A failed Ethernet or UM980 init is retried (every 30 s / 60 s) instead of
  stopping the program.
- The hardware watchdog (8 s) is enabled. The Python had it disabled in
  `wdt.py`. A stalled NTRIP core also triggers a reset.
- The config keys `signal_group`, `sbas_enabled`, `rtcm_interval`,
  `base_mode` (`time` / `fixed` with `base_lat` / `base_lon` / `base_alt`),
  `base_duration` and `base_pdop` are now applied. The Python ignored them and
  hard-coded the same defaults.
- `config_url` can only be set by provisioning, not by the remote config.
- The status page and telemetry gain firmware version, partition, uptime,
  RTCM frame/CRC counters and OTA state. Telemetry adds a `fw` field.
  `/status.json` serves the same data as JSON.
- The SHT40 CRC is checked. AGC is queried once per minute (the Python
  queried it twice).
- HTTPS is supported for all URLs, with certificate verification (see HTTPS
  below). The Python used TLS without verification.

## Network robustness

- **Random local ports** (hardware RNG, 49152–65535) for every outgoing TCP
  connection. A reboot without a clean close never reuses the 4-tuple of a dead
  session, which stale NAT or peer state would otherwise swallow.
- **No-ACK detection:** if the caster acknowledges nothing for 10 s while data
  is queued, the NTRIP session is reconnected on a fresh port. The reason is
  logged (`NTRIP stream ended: …`).
- **Path MTU:** the W5500 does no PMTU discovery (it sends with Don't-Fragment
  set and ignores ICMP "fragmentation needed"). The firmware clears DF on its
  TCP sockets, so IPv4 routers fragment instead of dropping. After an ACK
  stall with full-size segments in flight, it also reconnects with a smaller
  MSS (1460 → 1360 → 1200 → 536). The current value is in `/status.json`
  (`tcp_mss`).
- `tcp_mss` (remote config key, default 0 = automatic) pins the MSS for a
  known link, e.g. `1360` behind a VPN.

## GNSS auxiliary signals (1.1.0+)

| GPIO | Signal | Use |
|------|--------|-----|
| 2 | RTK_TPS (PPS) | 1 pulse/s, rising edge = start of the GNSS second (UM980: 20 ns RMS). Captured by interrupt. |
| 3 | RTK_STAT | High only for an RTK *fixed* solution, so normally 0 on a base. Shown as is. |
| 4 | RTK_RESET | UM980 reset (active low). High-Z normally, pulled low 500 ms to reset. Must read high at boot, otherwise resets are disabled. |
| 5 | RTK_EXTINT | UM980 EVENT input. Unused, left high-Z. |

The UM980 config check also enforces `CONFIG PPS ENABLE GPS POSITIVE 500000 1000 0 0`.

### UTC clock

Every minute the firmware waits for a PPS edge, queries `RECTIMEA`, and labels the
edge just before the `#RECTIMEA` line with its UTC second. The time is then extrapolated
from the latest PPS edge, valid for 60 s without PPS. It's shown on the status page,
in `/status.json` (`utc`) and in telemetry (`ts`, `null` when unknown). Measured
against an NTP-synced host, it agrees within the HTTP round-trip time (< 10 ms).

### GNSS watchdog

Decided once a minute (`gnss_logic.c` `wdog_step`, host-tested):

- **Reset the UM980** only when it's hung: no answer on COM1 for 3 consecutive
  minutes, or (receiver answering, PPS present) no RTCM on COM2 for 5 minutes and
  still none 5 minutes after a config re-check.
- **Never** for a missing PPS alone (antenna unplugged, no sky). That's reported as
  `no PPS (antenna/sky?)`.
- **Anti-loop:** no reset in the first 10 min after boot, ≥ 15 min between resets,
  and at most 3 until 24 h of continuous health. The budget is persisted, so
  reboots don't refill it. When it's exhausted the state reads `gave up`.
- `gnss_watchdog` (remote config, default `true`) turns the actions off; the state
  is still reported.

### UM980 firmware upgrade

The UM980 serial bootloader protocol: reset → `T@` trigger → menu `2` → XMODEM-1K-style
packets → `backup succeed` → menu `6`. This follows SparkFun's open-source
`UM980_Update_From_Array.ino`, with one difference: the receiver pauses up to
~6 s mid-transfer (flash erase), so ACKs are awaited up to 30 s. A transfer
aborted mid-way leaves the receiver on its old firmware (observed twice).

It uses the `um980` section of the firmware manifest (see "Firmware
updates"), plus `maint_window` from config.json: `"HH:MM-HH:MM"` in UTC.
The window may wrap midnight. Scheduled flashing only happens inside it,
and only with a valid UTC clock.

The regular `ota_interval_h` check and the boot-time check re-download the
remote config and check the manifest. They flash only inside `maint_window`.

The **Check for new firmware** button behaves differently:
1. It checks the manifest's UM980 part first. A newer package (or a forced one not
   yet applied) is downloaded, verified and **flashed right away**, outside
   the window. Whoever pressed it accepts the ~75 s of downtime.
2. Only when that is done does it run the RP2350 OTA check. That check
   reboots the device when there is an update, so the two never overlap.

Tested: a forced downgrade at 18:28 UTC (window 03:00–04:00) flashed
immediately. Publishing a UM980 upgrade and an RP2350 release together
upgraded the UM980 first, then installed and rebooted into the new RP2350
firmware.

Optional `um980` fields, for testing: `"force": true` allows a reinstall or
downgrade, and `"id"` lets a forced package be applied again:

- **Normal manifests** only ever upgrade (by build number). The package is
  downloaded into flash at 8 MiB (staging, 4 MiB), verified (size, SHA-256, `.pkg`
  magic and `UM980` product name), re-verified just before flashing, and then
  flashed at 921600 baud (115200 fallback). After flashing, the receiver config is
  re-checked and NTRIP resumes. Measured downtime: ~75 s.
- **`"force": true`** allows a reinstall or downgrade. Each forced package (file +
  `id`) is applied **once**. Publish it with a new `id` to apply it again.
- **Failures:**
  - A package that fails with the receiver back on its old firmware is never
    retried automatically.
  - If the receiver stays silent (e.g. stuck in its bootloader), the firmware
    retries the flash outside the window, up to 3 attempts in total, 10 min apart.
  - Attempts are recorded before they start, so a reboot mid-upgrade is noticed.

## Sockets, NTP server, download jitter, static IP fallback (1.2.0+)

W5500 socket plan (8 sockets, 16 KB TX + 16 KB RX):

| # | Use | TX/RX KB |
|---|-----|----------|
| 0 | DHCP | 1/1 |
| 1 | DNS | 1/1 |
| 2 | NTRIP (core 1) | 8/1 |
| 3 | status web server #1 | 2/2 |
| 4 | HTTP client (config, telemetry, OTA) | 1/8 |
| 5 | NTP server | 1/1 |
| 6 | status web server #2 | 2/2 |
| 7 | spare | 0/0 |

- **Web server:** two listeners on port 80, so two clients are served at once
  and a third is refused. A listener stuck in a closing state (client gone) is
  force-closed after 2 s. `/status.json` shows the state of all 8 sockets
  (`sockets`).
- **NTP server** (`"ntp_server": true` in the remote config, default off):
  stratum 1 on UDP 123, refid `PPS`.
  - The receive timestamp comes from the W5500 interrupt line (GPIO21, falling
    edge on the NTP socket's RECV event), so it's accurate to microseconds even
    though the socket is serviced every ~20 ms.
  - When the UTC clock isn't valid (no PPS), it answers with LI=3 / stratum 16,
    so clients ignore it.
- **Download jitter** (`"download_jitter_s"`, default 3600, max 86400): when a
  *scheduled* check finds a firmware update (RP2350 or UM980), the download
  starts after a random 0..N s delay (hardware RNG). A fleet doesn't hit the
  server at once. A later scheduled check keeps an already-pending download of
  the same version (it doesn't redraw the delay). The status page button is
  never delayed and replaces any pending download. The first check after boot
  (RP2350 at 60 s, UM980 at 3 min) isn't delayed either.
- **Static IP fallback:**
  - With `"dhcp": false`, if no connection to a host *outside the local
    subnet* succeeds for 5 min, the firmware switches to DHCP. Success means
    config, telemetry or OTA requests, or the NTRIP caster. LAN-only
    successes don't count, because a wrong gateway leaves the LAN reachable.
  - If no lease comes within 2 min, it returns to the static settings and only
    retries 30 min later.
  - The stored config isn't changed: the next boot starts static again.
  - An invalid static configuration goes to DHCP straight away.
  - `net_mode` in `/status.json` shows the current mode.

### Behaviour on a noisy LAN (tested with 1.2.8)

- **The W5500 filters most chatter in hardware.** The firmware never joins a
  multicast group, so mDNS/SSDP/LLMNR are dropped. The NTP and DNS sockets also
  block broadcasts (`SF_BROAD_BLOCK`). Only DHCP listens to broadcasts on its
  own port. ARP and ping are answered by the chip.
- **Measured:** 28,000 mDNS and broadcast packets in 60 s had no effect.
  Status page 12/12, NTP 12/12 at +0.08 ms, NTRIP unaffected.
- **Targeted junk at the board** (~1,600 packets/s of malformed UDP, including
  port 123, plus TCP garbage and silent connections on port 80):
  - No reset, NTRIP unaffected (0 overruns).
  - NTP keeps answering (29/30), within a few ms. A request whose arrival time
    can't be known exactly (queued behind other packets) is dropped rather than
    answered with a wrong time (`ntp_dropped` in `/status.json`).
  - The status page stays mostly unavailable while someone deliberately keeps
    both listeners busy. Garbage requests get a 2-line `400`, and silent
    connections are dropped after 2 s.
- **UDP sends never block.** ioLibrary's `sendto()` spins until ARP completes;
  DNS and NTP use a non-blocking send instead, and DNS is a built-in
  non-blocking client. A dead DNS server or a vanished NTP client can't stall
  a core or trigger the watchdog.

## Status dashboard (1.5.0+)

`http://<board>/` serves **Centipede-RTK MCU Base Status**, a dark dashboard
drawn entirely in SVG by vanilla JS, with no library and no external request.

- **Header:** logo, NTRIP connection and mountpoint, firmware and hardware
  ID, and a live UTC clock (GNSS time, ticking client-side).
- **LED row:** one card per LED, showing its current colour and meaning
  (LED2 blinks for "no PPS"), with a legend of all that LED's colours and
  the current one highlighted.
- **Key figures:**
  - satellites per constellation;
  - signal quality: average C/N0 of the tracked satellites (good ≥ 40,
    fair 32–40, poor < 32 dB-Hz), the best one, and how many are ≥ 35;
  - uptime;
  - RTCM throughput, frames, CRC errors and overruns;
  - caster.
- **Cards:**
  - **CPU:** per-core ring gauges with peaks, clock, MCU temperature, stack.
  - **AGC:** L1/L2/L5 bars with the "good < 10" line, and a warning when a
    band is bad.
  - **Level:** pitch/roll bullseye, with the 2° tolerance zone and the
    5-minute deltas.
  - **Environment:** board (SHT40) and MCU (RP2350) thermometers, humidity.
    - Thermometer scale: −10 to +90 °C, orange from 70 °C, red from 80 °C.
    - A marker shows 85 °C, the operating limit of both the RP2350
      (ambient) and the UM980.
    - The MCU sensor reads the silicon itself, whose absolute maximum is
      125 °C.
  - **Sky plot:** from the UM980 `SATSINFOA` log, polled once a minute;
    colour = constellation, size and opacity = C/N0.
  - **Timing:** UTC, PPS, period and deviation, RTK_STAT, NTP server.
  - **System:** UM980 version and update, GNSS watchdog, network, config
    source, partition, OTA, and the "Check for new firmware" button.
- **History:** RTCM throughput, AGC, CPU, temperatures, humidity, PPS
  deviation, satellites and average C/N0, over 10 min / 30 min / 1 h, with a hover readout.
  - The page polls `/status.json` every 10 s. CPU and RTCM figures change
    at that pace; AGC, satellites and temperatures are sampled by the
    firmware once a minute.
  - It keeps one sample per poll for 1 hour in the browser's `localStorage`
    (key `crtk:<hardware id>`, 360 samples, ~28 KB).
  - History therefore starts when a browser first opens the page, and is
    per browser.
  - Refused connections are retried a few times with a random delay. The
    board has two HTTP sockets, enough for one browser plus a stray
    connection.
- **Size:** one self-contained page of 38 KB, 14 KB gzipped, logo included.
  - It is a single file because the W5500 has no listen backlog: with both
    HTTP sockets busy, a third parallel request (separate CSS/JS files) would
    be refused.
  - It is served with `Content-Encoding: gzip` and an `ETag` (version +
    content hash), so a reload costs a `304` and zero bytes.
  - The build fails above 20 KB gzipped (`tools/build_web.py`).
- **The plain server-rendered page** (no JavaScript) is still at `/basic`.
- **New `/status.json` fields:**
  - base: `base_mode`, `base_lat`/`base_lon`/`base_alt`, `mountpoint`,
    `ntrip_server`;
  - system: `ota`, `ota_enabled`, `um980_model`;
  - LEDs: `led1_meaning`, `led2_meaning`, `led2_blink`;
  - network: `ip`, `gateway`;
  - GNSS: `agc_state`, `pitch`, `roll`, `pps_dev_us`, `sats`, and `sky`
    (`[[system, prn, az, el, cn0], …]`);
  - MCU: `mcu_temp`, `mcu_mhz`.


## CPU load (1.4.5+)

`/status.json` reports `cpu` (each core's busy share over the last 10 s
window, in %) and `cpu_peak` (the highest window since boot). The status page
shows them in the "CPU" row.

- **How it works:** `sleep_ms`/`sleep_us` are wrapped at link time
  (`src/cpu_load.c`). Every wait in the firmware goes through them, so busy
  means "not sleeping".
- **What counts as busy:** blocking I2C transfers. Interrupts that fire while
  a core sleeps count as idle; they are short.
- **Measured on 1.4.6, steady state (150 MHz):**
  - Core 0: ~12 %. That is mostly the IMU vibration and tilt sampling, i.e.
    I2C transfers.
  - Core 1 (NTRIP): ~5 %.
  - Startup peaks reach ~65 %.
- Before 1.4.6, the IMU data-ready wait polled the I2C status register
  without sleeping, which kept core 0 at 100 %.

## HTTPS (1.4.0+)

The HTTP client (remote config, telemetry, OTA and UM980 manifests and
downloads) accepts `http://` and `https://` URLs. HTTPS uses mbedTLS 3.6 from
the Pico SDK (`src/rtk_mbedtls_config.h`):

- **Protocol:** TLS 1.2 client: ECDHE with ECDSA or RSA certificates,
  AES-GCM or ChaCha20-Poly1305, SNI, extended master secret.
- **Certificates are always verified,** including the hostname (or IP SAN).
  - Trust store (`third_party/certs/`), plus an optional
    **`provision/ca.pem`** (one or more PEM certificates) for a private CA.
    The bundle is generated at build time (`tools/gen_ca_bundle.py`).

    | Root | Key | Expires | Source, SHA-256 fingerprint |
    |---|---|---|---|
    | ISRG Root X1 | RSA 4096 | 2035-06-04 | Debian Mozilla bundle, `96:BC:EC:…:08:C6` |
    | ISRG Root X2 | ECDSA P-384 | 2040-09-17 | Debian Mozilla bundle, `69:72:9B:…:14:70` |
    | Root YR | RSA 4096 | 2045-09-02 | letsencrypt.org, `E5:7B:7E:…:A8:6F` |
    | Root YE | ECDSA P-384 | 2045-09-02 | letsencrypt.org, `E1:4F:FC:…:56:66` |

    YR and YE are Let's Encrypt's "Generation Y" roots, issuing since
    January 2026. Their keys were checked against the cross-signatures from
    X1 and X2. Servers currently send chains like
    `leaf ← YE2 ← Root YE ← (cross-sign by X2, expires 2032-09-02)`. With YE
    as an anchor, verification stops at YE, so the device does not depend on
    the cross-signature.
  - Validity dates of the server certificate and intermediates are checked
    against the GNSS UTC clock. Without UTC (no PPS) only the date check is
    skipped; the chain and name are still verified.
  - **The trust anchors' own dates are not enforced.** They are trusted
    because they are compiled in, and a root expiring must not cut a device
    off from the update that replaces it.
  - **Instead, the build fails if an anchor expires within 5 years**
    (`CA_MIN_YEARS`, a CMake cache variable). The check runs whenever the
    trust store is regenerated, which includes every `VERSION` bump. When it
    fails, add the successor roots, and drop the old ones once no server
    needs them.
- **Config fallback to HTTP:** if `config_url` is `https://` and 3 attempts
  (2 s apart) all fail without any HTTP response (handshake or certificate
  failure, server down, ...), the same URL is fetched once with `http://`.
  - An explicit port is kept as is; the default port becomes 80.
  - Every later download tries HTTPS first again.
  - A server that answers over TLS, even with an error such as 404, does
    not trigger the fallback.
  - `config_source` in `/status.json` and the "Config" row of the status
    page show `https`, `http`, `http (HTTPS failed)` or `local`.
  - This keeps a device with a stale trust store reachable: its config can
    point `firmware_url` to a fixed firmware.
  - It also means someone who can block port 443 and intercept port 80 on
    the path can feed the device a config. Only `config.json` falls back;
    the OTA and UM980 manifests and downloads never do.
- **Cost:** about +145 KB of flash, and ~25–40 KB of heap during a connection.
  Hashing for the downloads stays on the hardware SHA-256 engine; TLS uses
  software SHA.
- **Core 0 stack:** core 0 runs on a 16 KB stack in RAM (`src/stack_mon.c`),
  guarded by the ARMv8-M stack limit register, so an overflow faults instead of
  corrupting memory. TLS handshakes peak at about 4 KB. The never-used part is
  reported as `stack_free` in `/status.json`. Core 1 has 4 KB.
- **Tested against:**
  - crtk.net (Let's Encrypt ECDSA chain): verified.
  - A private RSA CA: a 620 KB OTA image downloaded and installed over HTTPS.
  - A self-signed certificate: rejected ("not correctly signed by the
    trusted CA").
  - An expired certificate: rejected ("validity has expired").
  - An expired private root with a valid leaf: accepted (anchor dates are
    not enforced). The build refused this root until `CA_MIN_YEARS` was
    overridden.
  - Config fallback: an expired leaf was rejected 3 times, then the config
    was fetched over `http://` on the same URL. A 404 over TLS did not fall
    back. A working certificate brought `config_source` back to `https`.
