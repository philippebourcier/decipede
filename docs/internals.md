# Internals

## Flash layout

The board has 16 MB of flash:

| Region | Offset | Size | Content |
|---|---|---|---|
| PT | 0x000000 | 8 KB | Partition table (`partitions.json`) |
| App A | 0x002000 | 3064 KB | Application slot |
| App B | 0x300000 | 3072 KB | Application slot (linked to A) |
| Config | 0x600000 | 64 KB | Configuration records |
| (free) | 0x610000 | ~1.9 MB | |
| UM980 staging | 0x800000 | 4 MB | Downloaded UM980 firmware package |
| (free) | 0xC00000 | 4 MB | |

- **Bootrom partitions:** App A, App B and Config are partitions known to
  the bootrom.
- **Slot size:** the firmware image is ~330 KB, so the 3 MB slots leave
  about 9× headroom. The layout can only be changed over USB (BOOTSEL), so
  it is sized for the long term.
- **Config address:** the build reads the Config partition's start from
  `partitions.json`, so the firmware's storage always matches the partition
  table.
- **Config records:** four records of 2 sectors each, safe against power
  cuts:
  - the config JSON;
  - the OTA state;
  - the UM980 staging record;
  - the GNSS watchdog's reset budget.
- **UM980 staging area:** plain unpartitioned flash, at `GNSS_STAGE_OFFSET`
  in `src/flash_store.h`. `partitions.json` makes it readable and writable.
  An RP2350 update only ever writes the inactive application slot, so it
  can't touch a staged package.

## Identity

- **Hardware ID:** the RP2350's 64-bit unique chip ID, in hex.
- **Ethernet MAC:** a locally administered address, hashed from the full
  chip ID (splitmix64, 46 effective bits).
  - Each board keeps the same MAC across reboots and updates.
  - For 100,000 boards, the chance of any duplicate is ~0.007 %.
  - Logging `hardware_id` and the MAC at the factory catches the rest.

## Cores, stacks and watchdogs

- **Core 0:**
  - Runs the main loop: network, web server, sensors, GNSS control,
    updates.
  - Its 16 KB stack is in RAM, guarded by the ARMv8-M stack limit register,
    so an overflow faults instead of corrupting memory.
  - The part of the stack never used is reported as `stack_free` in
    `/status.json`.
- **Core 1:** runs NTRIP, with a 4 KB stack.
- **Watchdogs:**
  - The hardware watchdog (8 s) is always enabled.
  - A stalled NTRIP core also triggers a reset.
- **Init retries:** a failed Ethernet or UM980 initialisation is retried
  every 30 s / 60 s.

## CPU load

`/status.json` reports two values:
- `cpu`: each core's busy share over the last 10 s, in %;
- `cpu_peak`: the highest 10 s window since boot.

**How it is measured:**
- `sleep_ms` and `sleep_us` are wrapped at link time (`src/cpu_load.c`).
- Every wait in the firmware goes through them, so "busy" means "not
  sleeping". This includes blocking I2C transfers.
- Interrupts that fire while a core sleeps count as idle; they are short.

**Typical steady state at 150 MHz:**
- Core 0: ~12 %, mostly the IMU vibration and tilt sampling (I2C
  transfers).
- Core 1 (NTRIP): ~5–7 %.
- Startup peaks reach ~65 %.

## Source map

| Module | Role |
|---|---|
| `main.c` | Boot sequence and main loop |
| `config.c`, `flash_store.c` | Remote config, persistent storage |
| `http_client.c`, `tls.c` | HTTP(S) client |
| `http_server.c`, `web/` | Dashboard, `/basic`, `/status.json` |
| `net.c`, `dns_proto.c`, `wiz.h` | W5500, DHCP, DNS, static IP fallback |
| `ntrip.c`, `rtcm.c` | NTRIP client on core 1, RTCM framing and CRC |
| `um980.c`, `gnss_io.c`, `gnss_logic.c` | Receiver configuration, logs, parsers |
| `gnss_watchdog.c`, `utc.c` | GNSS health watchdog, PPS/UTC clock |
| `ota.c`, `fw_manifest.c`, `um980_fw.c` | Firmware manifest, RP2350 OTA, UM980 flashing |
| `ntp_server.c`, `ntp_proto.c` | NTP server |
| `lsm6dsv.c`, `sht4x.c`, `mcu_temp.c` | Sensors |
| `led.c`, `wdt.c`, `idle.c`, `cpu_load.c`, `stack_mon.c` | LEDs, watchdog, idle, CPU and stack monitoring |

## Differences from uPyRTKBase

decipede keeps uPyRTKBase's boot sequence, LED codes, config server protocol
and telemetry. It differs in these ways:

| Area | uPyRTKBase | decipede |
|---|---|---|
| Updates | none | A/B OTA with rollback; UM980 firmware updates |
| NTRIP | Gives up after 120 / 3600 attempts | Retries forever; drops stale RTCM |
| Failed Ethernet or UM980 init | Stops the program | Retried |
| Hardware watchdog | Disabled | Enabled (8 s), plus an NTRIP core check |
| Config keys `signal_group`, `sbas_enabled`, `rtcm_interval`, `base_mode`, `base_duration`, `base_pdop` | Ignored (hard-coded) | Applied |
| `config_url` | Can be set by the remote config | Provisioning only |
| TLS | No certificate verification | Full verification |
| Status | HTML page | Dashboard, `/basic`, `/status.json`; telemetry gains `fw`, `ts`, `pps`, `rtk_stat`, `um980_fw`, `gnss_resets` |
| SHT40 | CRC not checked | CRC checked |
| AGC | Queried twice a minute | Once a minute |
