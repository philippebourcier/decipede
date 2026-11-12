# GNSS

## UM980 configuration

The firmware configures the receiver at every boot, so the receiver's saved
configuration doesn't matter.
- **COM1** is the control port: commands, logs and firmware updates.
- **COM2** carries the RTCM stream for the caster.
- Both run at 115200 baud.

It checks and, when needed, sets:
- **Base mode,** from `config.json`:
  - `time` runs a self-survey: `MODE BASE 1 TIME <base_duration> <base_pdop>`.
  - `fixed` uses the known position `base_lat`, `base_lon`, `base_alt`.
    Coordinates out of range fall back to self-survey.
- **Signal group** (`signal_group`, default 2) and **SBAS**
  (`sbas_enabled`). Changing the signal group needs a receiver reboot, which
  is skipped when the value is already right.
- **GPS L2P(Y)** (`gps_l2p_enabled`, default `true`): `false` sends
  `MASK L2P`, `true` sends `UNMASK L2P` if the mask is set.
  See [GPS L2 code](#gps-l2-code).
- **PPS:** `CONFIG PPS ENABLE GPS POSITIVE 500000 1000 0 0`.
- **The RTCM 3 messages on COM2:**

| Messages | Content | Interval |
|---|---|---|
| 1005, 1006, 1033 | Station position and antenna/receiver description | 30 s, 30 s, 10 s |
| 1019, 1020, 1042, 1044, 1045, 1046 | Ephemerides: GPS, GLONASS, BeiDou, QZSS, Galileo (F/NAV, I/NAV) | 10 s |
| 1077, 1087, 1097, 1107, 1117, 1127 | MSM7 observations: GPS, GLONASS, Galileo, SBAS, QZSS, BeiDou | `rtcm_interval` (1 s) |

**RTCM checks:**
- The firmware checks the CRC of every RTCM frame it relays.
- Frame and CRC error counts are in `/status.json`.

**AGC:**
- The receiver's AGC is queried once a minute on L1, L2 and L5.
- A degraded band turns LED2 blue.
- The values are sent with telemetry, so jamming or a failing antenna shows
  up.

**Satellites:**
- `SATSINFOA` is queried once a minute.
- It feeds the satellite counts, the sky plot and the signal-quality figure
  on the dashboard.
- The query is skipped while the UM980 is being flashed.

## GPS L2 code

By default the UM980 tracks two codes on GPS L2 and both go into message
1077: L2P(Y) (RINEX `2W`) on every satellite, and L2C on the satellites that
transmit it.

**Why turn L2P(Y) off:**
- The UM980's L2P(Y) pseudorange carries an offset of the order of a metre.
  It differs from unit to unit and moves over days. Being common to all
  satellites, it cancels as long as one code is used on GPS L2.
- It stops cancelling when software mixes the two codes, L2P(Y) for some
  satellites and L2C for the others. Decoders that keep one code per band
  take L2P(Y) first when it is present (RTKLIB's code priority), so a UM980
  is read on `2W` and a UM982, which has no L2P(Y), on L2C.
- With `gps_l2p_enabled: false` the stream carries the same GPS signals as a
  UM982's, and no mix is possible downstream.

**Cost:** the GPS satellites without L2C (the remaining block IIR) lose
their L2 observations.

**Compatibility:** the key defaults to `true`, so a station whose
`config.json` doesn't carry it behaves as before. The mask is stored by
`SAVECONFIG` and is checked at every boot (`MASK` query), so going back to
`true` restores L2P(Y).

## Auxiliary signals

| GPIO | Signal | Use |
|---|---|---|
| GP2 | PPS | One pulse per second; the rising edge marks the start of the GNSS second (UM980: 20 ns RMS). Captured by interrupt. |
| GP3 | RTK_STAT | High only with an RTK *fixed* solution, so normally low on a base. Reported as is. |
| GP4 | RESET_N | Receiver reset, active low. High-Z normally; pulled low for 500 ms to reset. It must read high at boot, otherwise resets are disabled. |
| GP5 | EVENT | Unused, left high-Z. |

Wiring details are in [HARDWARE.md](../HARDWARE.md).

## UTC clock

**Synchronisation:** once a minute, the firmware:
1. waits for a PPS edge;
2. queries `RECTIMEA`;
3. labels the edge just before the `#RECTIMEA` line with its UTC second.

**Between syncs,** the time is extrapolated from the latest PPS edge. It
stays valid for 60 s without PPS.

**Where it is used:**
- the dashboard, `/status.json` (`utc`) and telemetry (`ts`, `null` when
  unknown);
- certificate date checks;
- the maintenance window;
- the NTP server.

Measured against an NTP-synced host, it agrees within the HTTP round-trip
time (< 10 ms).

## No PPS

**When it triggers:** the receiver answers, but there has been no PPS for
60 s. That means no satellite lock, so no corrections: the antenna is
disconnected, or has no sky view.

**What the station does:**
- LED2 blinks red at 1 Hz, and `led2` in `/status.json` reads `RED_BLINK`.
- This overrides green, blue and pink, but not solid red (receiver failure).
- It clears by itself when PPS returns.

## GNSS watchdog

The watchdog decides once a minute. The logic is in
`gnss_logic.c`/`wdog_step` and is host-tested.

- **Reset the UM980** only when it is hung:
  - no answer on COM1 for 3 consecutive minutes;
  - or no RTCM on COM2 for 5 minutes, with the receiver answering and PPS
    present, and still none 5 minutes after a configuration re-check.
- **Never** for a missing PPS alone: an unplugged antenna or no sky view is
  reported as `no PPS (antenna/sky?)`, not "fixed" by resets.
- **Anti-loop:**
  - no reset in the first 10 min after boot;
  - at least 15 min between resets;
  - at most 3 resets until the receiver has been healthy for 24 h.
  - The budget is stored in flash, so a reboot doesn't refill it. When it
    runs out, the state reads `gave up`.
- **`gnss_watchdog: false`** in `config.json` disables the resets. The state
  is still reported.
