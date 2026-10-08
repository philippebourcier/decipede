# Status dashboard

`http://<station>/` serves the status dashboard. It is a dark single page,
drawn entirely in SVG by vanilla JavaScript, with no library and no external
request.

## Layout

- **Header:**
  - logo and title (see `BRAND_*` in the README's Provisioning section);
  - NTRIP connection and mountpoint;
  - firmware version and hardware ID;
  - a live UTC clock: GNSS time, ticking client-side.
- **LED row:** one card per LED. It shows the LED's current colour and
  meaning, with a legend of all its colours and the current one
  highlighted. LED2 blinks for "no PPS".
- **Key figures:**
  - satellites per constellation;
  - **signal quality:** the average C/N0 of the tracked satellites (good
    ≥ 40, fair 32–40, poor < 32 dB-Hz), the best one, and how many are
    ≥ 35;
  - uptime;
  - RTCM throughput, frames, CRC errors and overruns;
  - the caster.
- **Cards:**
  - **Sky plot:** from the UM980 `SATSINFOA` log, polled once a minute.
    Colour shows the constellation; size and opacity show C/N0.
  - **AGC:** L1/L2/L5 bars with the "good < 10" line, and a warning when a
    band is bad.
  - **Level:** a pitch/roll bullseye, with the 2° tolerance zone and the
    5-minute deltas.
  - **Environment:** board (SHT40) and MCU (RP2350) thermometers, and
    humidity.
    - The scale runs from −10 to +90 °C: orange from 70 °C, red from
      80 °C.
    - A marker shows 85 °C, the operating limit of both the RP2350
      (ambient) and the UM980.
    - The MCU sensor reads the silicon itself, whose absolute maximum is
      125 °C.
  - **Timing:** UTC, PPS count, period and deviation, RTK_STAT, and the NTP
    server.
  - **System:**
    - UM980 version and update state, GNSS watchdog;
    - network, config source, partition, OTA state;
    - the **Check for new firmware** button, shown when `firmware_url` is
      set.
  - **CPU:** per-core ring gauges with peaks, clock, MCU temperature, and
    free stack.
- **History:** a chart with a hover readout, over 10 min, 30 min or 1 h. It
  shows:
  - RTCM throughput, AGC and CPU;
  - temperatures and humidity;
  - PPS deviation;
  - satellites and average C/N0.

## Data and history

- **Polling:** the page polls `/status.json` every 10 s.
  - CPU and RTCM figures change at that pace.
  - AGC, satellites and temperatures are sampled by the firmware once a
    minute.
- **Storage:** one sample per poll is kept for 1 hour in the browser's
  `localStorage`: key `crtk:<hardware id>`, 360 samples, ~28 KB.
  - History starts when a browser first opens the page, and is kept per
    browser.
  - Without storage (e.g. private browsing), the page keeps its history in
    memory.
- **Refused connections:** these are retried a few times with a random
  delay. The board has two HTTP sockets: enough for one browser plus a
  stray connection.

## Size and serving

- **Size:** the dashboard is one self-contained page of ~38 KB, ~14 KB
  gzipped, logo included.
- **Why a single file:** the W5500 has no listen backlog. With both HTTP
  sockets busy, a third parallel request, such as a separate CSS or JS
  file, would be refused.
- **Caching:** the page is served with `Content-Encoding: gzip` and an
  `ETag` (firmware version + content hash). A reload costs a `304` and zero
  bytes.
- **Build:** `tools/build_web.py` turns `web/index.html` and the logo into a
  C array at build time. The build fails if the page exceeds 20 KB gzipped.
- **No JavaScript needed:** a plain server-rendered page is available at
  `/basic`.

## `/status.json`

All the values the dashboard shows, as one JSON object. A value that is
unknown (no sensor, no reading yet) is `null`.

| Group | Fields |
|---|---|
| Identity | `hw`, `fw`, `partition`, `uptime`, `um980_model`, `um980_fw` |
| LEDs | `led1`, `led2`, `led1_meaning`, `led2_meaning`, `led2_blink` |
| NTRIP and RTCM | `ntrip`, `ntrip_server`, `mountpoint`, `ntrip_bytes`, `rtcm_frames`, `rtcm_crc_errors`, `rtcm_overruns`, `tcp_mss` |
| Base | `base_mode`, `base_lat`, `base_lon`, `base_alt` |
| Satellites | `sats` (`{"total": n, "GPS": n, …}`), `sky` (`[[system, prn, az, el, cn0], …]`) |
| AGC | `agc_l1`, `agc_l2`, `agc_l5`, `agc_state` (`good` / `bad` / `unknown` per band) |
| Timing | `utc`, `pps`, `pps_count`, `pps_period_us`, `pps_dev_us`, `rtk_stat` |
| IMU | `pitch`, `roll`, `pitch_delta`, `roll_delta`, `rms_max_delta`, `vibrating`, `level` |
| Environment | `temperature`, `humidity`, `mcu_temp` |
| GNSS health | `gnss_watchdog`, `gnss_resets` |
| Updates | `ota`, `ota_enabled`, `gnss_fw` |
| Network | `net_mode`, `ip`, `gateway`, `config_source`, `sockets` |
| NTP | `ntp_server`, `ntp_requests`, `ntp_dropped` |
| MCU | `cpu`, `cpu_peak`, `mcu_mhz`, `stack_free` |
