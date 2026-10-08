<p align="center">
  <img src="web/logo.svg" width="96" alt="decipede logo">
</p>

<h1 align="center">decipede</h1>

<p align="center">
  <b>Open-source firmware for RTK GNSS base stations built on the RP2350 and the Unicore UM980.</b><br>
  Plug in Ethernet and an antenna: the station configures itself, streams RTCM corrections to an NTRIP caster and keeps itself up to date.
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-BSD--3--Clause-blue" alt="License: BSD-3-Clause"></a>
  <img src="https://img.shields.io/badge/MCU-RP2350-c51a4a" alt="MCU: RP2350">
  <img src="https://img.shields.io/badge/GNSS-Unicore%20UM980-0a7d8c" alt="GNSS: Unicore UM980">
  <img src="https://img.shields.io/badge/Pico%20SDK-2.3.1-555" alt="Pico SDK 2.3.1">
  <img src="https://img.shields.io/badge/language-C11-555" alt="Language: C11">
</p>

---

decipede runs on a **WIZnet W5500-EVB-Pico2** (RP2350 + W5500 Ethernet) wired
to a **Unicore UM980** triple-band receiver. It turns that pair into an
unattended, fleet-managed base station for an RTK network: the station
fetches its settings from a central `config.json`, sets up the receiver,
sends corrections to the caster, reports its health, and installs updates
for **both** the RP2350 and the UM980 over the network, with automatic
rollback.

It is a C rewrite of [uPyRTKBase](https://github.com/philippebourcier/uPyRTKBase)
(MicroPython), and keeps its boot sequence, LED codes, config server protocol and
telemetry.

## Contents

- [Features](#features)
- [Hardware](#hardware)
- [Getting started](#getting-started)
- [Configuration](#configuration)
- [Operation](#operation)
- [Firmware updates](#firmware-updates)
- [Documentation](#documentation)
- [Repository layout](#repository-layout)
- [Contributing](#contributing)
- [License and acknowledgements](#license-and-acknowledgements)

## Features

**GNSS base station**
- Configures the UM980 at every boot: self-survey (`time`) or known position
  (`fixed`) base mode, signal group, SBAS, and the RTCM 3 message set
  (MSM7 observations for GPS, GLONASS, Galileo, BeiDou, QZSS and SBAS, plus
  ephemerides and station information).
- Streams RTCM to an NTRIP caster from a dedicated core, with reconnection,
  stall detection and path-MTU handling for awkward networks.
- GNSS health watchdog: resets a hung receiver, but never because the
  antenna is unplugged, and never in a loop.

**Fleet management**
- **One URL in the firmware.** The station fetches `config.json` at boot.
  The caster, credentials, telemetry, firmware server and maintenance window
  all come from it.
- **Over-the-air updates for both chips** from one manifest:
  - RP2350: A/B slots with the bootrom's *try-before-you-buy*. An image that
    crashes, or never reaches the network, is rolled back automatically.
  - UM980: the receiver firmware is staged in flash, verified, and flashed
    inside a maintenance window.
  - Updates are spread over time across the fleet (random delay).
- **Telemetry** every 5 minutes: AGC, temperature, humidity, tilt, timing and
  firmware versions.
- **HTTPS everywhere,** with certificate verification and a trust store built
  to outlive the roots it ships with.

**On site**
- **Status dashboard** at `http://<station>/`, in one 14 KB page drawn in SVG:
  - satellites and sky plot, AGC, signal quality, RTCM throughput;
  - CPU, temperatures, level, and one hour of history.
  - A plain HTML version is at `/basic`, and the raw data at `/status.json`.
- **Two RGB status LEDs** for network and GNSS state.
- **Optional sensors:** an IMU raises a vibration or tilt alarm, and a
  temperature and humidity sensor watches the enclosure.
- **Stratum-1 NTP server** disciplined by the receiver's PPS (optional).
- **Robust by design:**
  - hardware watchdog;
  - power-cut-safe configuration storage;
  - DHCP fallback when a static IP is wrong;
  - tested against noisy and hostile LANs.

## Hardware

| Part | Role |
|---|---|
| WIZnet W5500-EVB-Pico2 (16 MB flash) | MCU (RP2350A) and Ethernet |
| Unicore UM980 module or breakout | Triple-band RTK GNSS receiver |
| Triple-band GNSS antenna (L1/L2/L5) | |
| LSM6DSV16X, SHT40 breakouts | Optional: tilt/vibration, temperature/humidity |
| 2 × common-anode RGB LED, push button | Status and reboot |
| WIZnet WIZPoE-P1 | Optional: Power over Ethernet |

The full parts list, wiring diagram, GPIO map and power budget are in
**[HARDWARE.md](HARDWARE.md)**.

## Getting started

### 1. Build

```sh
git clone https://github.com/philippebourcier/decipede.git
cd decipede
cp provision/device.env.example provision/device.env
$EDITOR provision/device.env        # set CONFIG_URL to your config.json
./build.sh                          # -> build/rtkbase.uf2, build/partitions.uf2
```

**Requirements:** git, CMake ≥ 3.19, make, a host C/C++ compiler, Python 3,
curl, tar and xz. Root access isn't needed.

**What the first run does** (`tools/setup.sh`): it initialises the submodules
(only the parts of the Pico SDK the build needs),
downloads the Arm GNU toolchain 14.3 into `.deps/` (verified against Arm's
SHA-256), and builds picotool. Later runs only rebuild what changed.

**Build options:**

| Variable | Effect |
|---|---|
| `JOBS=3 ./build.sh` | Limit parallel jobs (default: all CPUs). |
| `PICO_TOOLCHAIN_PATH=/path ./build.sh` | Use an existing Arm toolchain. |
| `cmake -B build-x -DPROVISION_FILE=other.env` | Build for another deployment. |

**Platforms:**
- Linux x86_64 is tested. The toolchain download also covers Linux aarch64
  and macOS.
- Builds are reproducible: the same commit and `device.env` give a
  byte-identical `rtkbase.uf2`.

### 2. Flash

This needs USB once. Every later update goes over the network.

1. Hold **BOOTSEL** while plugging the board into USB. A drive named
   `RP2350` appears.
2. Copy `build/partitions.uf2` to the drive. The board reboots into
   BOOTSEL again: wait for the drive to come back.
3. Copy `build/rtkbase.uf2` to the drive. The board reboots into decipede.

Both files are needed: the firmware expects the partition layout that
`partitions.uf2` installs. On a board that ran MicroPython, you can also
enter BOOTSEL with `mpremote exec "import machine; machine.bootloader()"`.
If the old firmware starts again after step 2, re-enter BOOTSEL before
step 3.

To go back to MicroPython: enter BOOTSEL, erase the flash with
`picotool erase`, then copy a MicroPython UF2.

### 3. First boot

Connect Ethernet and the antenna. The station then:

1. gets an address over DHCP;
2. downloads `config.json` from `CONFIG_URL` and stores it in flash;
3. configures the UM980 and starts streaming RTCM to the caster.

LED1 turns green when the caster is connected. Open `http://<station>/` to
see the dashboard.

**Identifying stations:**
- Each station is identified by its **hardware ID**, the RP2350's 64-bit
  chip ID in hex. It is shown on the dashboard, and sent with every config
  request and telemetry report.
- The Ethernet MAC is a stable, locally administered address derived from
  the same ID. Logging both per board makes stations easy to track down.

## Configuration

Configuration has two layers:
- **Build time:** the config URL and the branding, baked into the firmware.
- **Run time:** everything else, in a `config.json` served by your server.

### Provisioning (build time)

| File | In git | Purpose |
|---|---|---|
| `provision/defaults.env` | yes | Project defaults: the Decipede branding. A network forking decipede edits this file. |
| `provision/centipede.env` | yes | Example of a network's branding, to copy into `device.env`. |
| `provision/device.env` | no | One deployment's values, overriding `defaults.env`. Start from `device.env.example`. |
| `provision/ca.pem` | no | Optional extra root certificates (PEM), e.g. a private CA. |

| Key | Purpose |
|---|---|
| `CONFIG_URL` | The `config.json` URL, and the only URL in the firmware. The station appends `?b=<hardware id>`. `config.json` can't change it, so a bad config can't cut a station off. |
| `BRAND_NAME`, `BRAND_TAGLINE` | Dashboard title and subtitle. |
| `BRAND_LOGO` | Dashboard logo and favicon: a small SVG, path relative to the repository. |

**Checks at build time:**
- The build stops on unknown keys, and on values containing quotes,
  backslashes or `< >`.
- The logo is embedded in the dashboard, which must stay under 20 KB
  gzipped: keep it to a few KB.

### config.json (run time)

The station fetches `CONFIG_URL?b=<hardware id>` at boot and at every update
check, so a server can return per-station settings. Over HTTPS, it falls back
to HTTP after 3 failed attempts, so a station with an outdated trust store
can still be reached (see [docs/https.md](docs/https.md)).

```json
{
  "ntrip_server": "caster.example.org",
  "ntrip_port": 2101,
  "ntrip_mountpoint": "MYBASE",
  "ntrip_user": "mybase",
  "ntrip_password": "secret",
  "telemetry_url": "https://example.org/telemetry",
  "firmware_url": "https://example.org/fw/manifest.json",
  "maint_window": "03:00-04:00"
}
```

| Key | Default | Meaning |
|---|---|---|
| `ntrip_server`, `ntrip_mountpoint`, `ntrip_user`, `ntrip_password` | — | NTRIP caster and credentials. Required for streaming. |
| `ntrip_port` | `2101` | Caster port. |
| `telemetry_url` | — | URL that receives a telemetry POST every 5 min. Unset: no telemetry. |
| `firmware_url` | — | Firmware manifest for both chips (see [Firmware updates](#firmware-updates)). Unset: no updates, and no update button. |
| `maint_window` | — | UTC window for scheduled UM980 flashing, `"HH:MM-HH:MM"`; it may wrap midnight. Unset: only the button flashes the UM980. |
| `ota_interval_h` | `6` | Hours between config refreshes and update checks. |
| `download_jitter_s` | `3600` | A scheduled update starts after a random 0…N s delay (max 86400). |
| `base_mode` | `"time"` | `"time"`: self-survey. `"fixed"`: known position from `base_lat`, `base_lon` (degrees) and `base_alt` (m). |
| `base_duration`, `base_pdop` | `60`, `1` | Self-survey duration (s) and PDOP limit. |
| `signal_group` | `2` | UM980 `CONFIG SIGNALGROUP` value. |
| `sbas_enabled` | `true` | SBAS tracking. |
| `rtcm_interval` | `1` | Interval of the MSM7 observation messages (s). |
| `dhcp` | `true` | `false` uses `ip`, `subnet`, `gateway`, `dns` (dotted quads). DHCP takes over if the static settings can't reach the Internet. |
| `tcp_mss` | `0` | TCP MSS; 0 is automatic. Pin it for a known link, e.g. `1360` behind a VPN. |
| `gnss_watchdog` | `true` | Allow the GNSS watchdog to reset the receiver. |
| `ntp_server` | `false` | Serve stratum-1 NTP on UDP 123. |

**Notes:**
- Any URL may be `http://` or `https://`.
- Keys missing from a later `config.json` keep their stored values.
- **Limits:** the file must be under 4 KB. URLs can be up to 191 bytes, and
  NTRIP strings up to 63.

## Operation

### Web interface

| Endpoint | Content |
|---|---|
| `GET /` | Status dashboard (gzipped, cached by ETag). |
| `GET /basic` | Plain status page with no JavaScript. |
| `GET /status.json` | All status values as JSON. |
| `POST /ota/check` | Same as the dashboard's **Check for new firmware** button. |

The dashboard is described in [docs/dashboard.md](docs/dashboard.md).

### Telemetry

When `telemetry_url` is set, the station POSTs a JSON object to it every 5
minutes. The object has these fields:
- `hw`: hardware ID; `fw`: firmware version; `um980_fw`: receiver version.
- `ts`: UTC timestamp from GNSS, `null` while unknown.
- `temperature`, `humidity`, `agc_l1`, `agc_l2`, `agc_l5`: 5-minute
  averages.
- `rms_max_delta`, `pitch_delta`, `roll_delta`: IMU vibration and tilt.
- `pps`, `rtk_stat`, `gnss_resets`: GNSS timing and watchdog state.

### LEDs

| Colour | LED1: network | LED2: GNSS / antenna |
|---|---|---|
| Orange | Booting, or NTRIP reconnecting | Booting |
| Cyan | Ethernet up, downloading config | UM980 up, checks pending |
| Yellow | Remote config failed, using the stored one | UM980 initialising |
| **Green** | **Config downloaded, NTRIP connected** | **AGC good, level, no vibration** |
| Blue | — | AGC degraded on one or more bands |
| Pink | — | Vibrating or tilted (IMU alarm) |
| Purple | Downloading an RP2350 update | — |
| Purple, both LEDs | Installing an update (RP2350 or UM980) | Installing an update (RP2350 or UM980) |
| Red | Ethernet init failed, or NTRIP reconnect failed | UM980 init failed, or AGC check error |
| Red, blinking | — | No PPS: no satellite lock for 60 s (antenna or sky view) |

### Button

Hold the button for more than 3 seconds to reboot the station.

## Firmware updates

One JSON manifest, at `firmware_url`, publishes firmware for both chips. Each
section is optional:

```json
{
  "rp2350": {"version": "1.8.2", "url": "https://example.org/fw/rtkbase-1.8.2.uf2",
             "size": 669184, "sha256": "…"},
  "um980":  {"model": "UM980", "version": "R4.10Build20739",
             "url": "https://example.org/fw/UM980_R4.10Build20739.pkg",
             "size": 3038408, "sha256": "…"}
}
```

**When stations check:** shortly after boot, every `ota_interval_h` hours,
and when someone presses **Check for new firmware** on the dashboard.

**What they install:**
- Only versions newer than the installed one.
- **RP2350:** a new version is installed as soon as it is found.
- **UM980:** a new version is flashed inside `maint_window`.
- **Fleet spreading:** scheduled checks start the download after a random
  delay of up to `download_jitter_s`.
- **The button** skips the delay and the window. It flashes the UM980
  first, then checks the RP2350.

**Publishing a release:**
1. Bump `VERSION` and run `./build.sh`.
2. Upload `build/rtkbase.uf2` under a versioned name.
3. Update the manifest. `size` is the file size in bytes; `sha256` is the
   lowercase hex digest of the file.

**Checks on the station:**
- Every download is checked against the manifest's size and SHA-256.
- RP2350 images are also hash-checked by the bootrom.
- An image that fails its trial boot or health check is rolled back.

The whole process is described in
[docs/firmware-updates.md](docs/firmware-updates.md), including UM980
flashing, failure handling and the security model.

## Documentation

| Document | Covers |
|---|---|
| [HARDWARE.md](HARDWARE.md) | Parts, wiring, GPIO map, power, PoE, using a stock EVB |
| [docs/firmware-updates.md](docs/firmware-updates.md) | Manifest, RP2350 A/B updates and rollback, UM980 flashing, scheduling, security |
| [docs/dashboard.md](docs/dashboard.md) | Status dashboard, `/status.json` fields |
| [docs/gnss.md](docs/gnss.md) | UM980 setup, RTCM output, PPS and UTC clock, GNSS watchdog |
| [docs/networking.md](docs/networking.md) | Sockets, NTRIP robustness, NTP server, static IP fallback, behaviour on noisy LANs |
| [docs/https.md](docs/https.md) | TLS, trust store and root expiry, HTTP fallback for the config |
| [docs/internals.md](docs/internals.md) | Flash layout, CPU load, stacks and watchdogs, source map, differences from uPyRTKBase |

## Repository layout

```
build.sh          one-step build (fetches dependencies on first run)
src/              firmware sources
web/              status dashboard (index.html, logo.svg), embedded gzipped at build time
boards/           board definition (W5500-EVB-Pico2, 16 MB flash)
partitions.json   flash partition table
provision/        build-time settings: defaults.env, device.env.example, example network profile
tools/            build helpers: setup.sh, build_web.py, gen_ca_bundle.py
third_party/      jsmn JSON parser, TLS root certificates
lib/              submodules: pico-sdk 2.3.1, WIZnet ioLibrary_Driver, picotool 2.3.1
docs/             detailed documentation
VERSION           firmware version (MAJOR.MINOR.PATCH)
```

The libraries are used unmodified, at the commits pinned by the submodules.
Workarounds for their known bugs live in `src/`.

## Contributing

Issues and pull requests are welcome.
- Keep changes focused.
- Build with `./build.sh` before submitting.
- Describe how you tested on hardware.
- A change that alters the flash layout or the `config.json`/manifest
  formats needs a note on compatibility with stations already deployed.

## License and acknowledgements

decipede is released under the [BSD 3-Clause License](LICENSE).

It builds on:
- [uPyRTKBase](https://github.com/philippebourcier/uPyRTKBase), the
  MicroPython original;
- the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk), with
  TinyUSB and Mbed TLS;
- [picotool](https://github.com/raspberrypi/picotool);
- WIZnet's [ioLibrary_Driver](https://github.com/Wiznet/ioLibrary_Driver);
- [jsmn](https://github.com/zserge/jsmn);
- SparkFun's open-source UM980 update example, for the receiver's bootloader
  protocol.
