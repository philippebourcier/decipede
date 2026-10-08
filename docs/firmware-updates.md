# Firmware updates

decipede updates two chips over the network:
- **the RP2350**, which runs decipede itself;
- **the UM980**, which runs Unicore's receiver firmware.

Both are published in one manifest, whose URL is `firmware_url` in
`config.json`.

## The manifest

```json
{
  "rp2350": {"version": "1.8.2", "url": "https://example.org/fw/rtkbase-1.8.2.uf2",
             "size": 669184, "sha256": "…"},
  "um980":  {"model": "UM980", "version": "R4.10Build20739",
             "url": "https://example.org/fw/UM980_R4.10Build20739.pkg",
             "size": 3038408, "sha256": "…"}
}
```

| Field | Meaning |
|---|---|
| `version` | RP2350: `MAJOR.MINOR.PATCH`. UM980: must contain the build, e.g. `R4.10Build20739`; versions are compared by build number. |
| `url` | `http://` or `https://`, at most 191 bytes. |
| `size` | File size in bytes. |
| `sha256` | Lowercase hex SHA-256 of the file. |
| `model` | UM980 only: must be `"UM980"`. |
| `force`, `id` | UM980 only, for testing: see [Forced packages](#forced-packages). |

- **Sections are optional:** publish only the chips that have a release.
- **Size limit:** the manifest itself must stay under 2 KB.
- **The request** carries
  `?b=<hardware id>&fw=<RP2350 version>&um980=<UM980 version>`. A server can
  use these to stage a rollout, e.g. answer only some stations with the new
  release.
- **Caching:** the manifest is downloaded once per check and cached for 60 s,
  so the UM980 and RP2350 checks share one download. The button always
  fetches it fresh.

## Publishing a release

**RP2350:**
1. Bump `VERSION`, then run `./build.sh`.
2. Upload `build/rtkbase.uf2` under a versioned name, e.g.
   `rtkbase-1.8.3.uf2`.
3. Update the `rp2350` section: the version, the URL, the size and the
   `sha256sum` of the file.

**UM980:**
1. Upload the genuine Unicore `.pkg` file (4 MB or less).
2. Update the `um980` section.

Stations only ever move to a **newer** version. To withdraw a release,
publish a newer one.

## When stations check

| Trigger | RP2350 | UM980 |
|---|---|---|
| After boot | ~60 s, no delay | ~3 min, no delay; flashes only inside `maint_window` |
| Every `ota_interval_h` hours (default 6) | after a random delay of 0…`download_jitter_s` | same delay, then flashes inside `maint_window` |
| **Check for new firmware** button | after the UM980, no delay | first, flashed **right away** |

- **Scheduled checks** also re-download `config.json`.
- **Random delay:** `download_jitter_s` defaults to 3600 s, so a fleet
  doesn't hit the server all at once. A later scheduled check keeps a
  download that is already pending for the same version; it doesn't draw
  a new delay.
- **The button** replaces any pending download. It handles the UM980 first,
  then the RP2350. The RP2350 update ends in a reboot, so the two never
  overlap.
- **Who presses the button accepts the downtime:** about 75 s for a UM980
  flash, a few seconds for an RP2350 reboot.

## RP2350: A/B updates with rollback

The flash holds two application slots (see [internals.md](internals.md#flash-layout)).
The running image never writes its own slot.

1. **Download:**
   - The UF2 is streamed into the *inactive* slot, and its SHA-256 is
     computed on the fly.
   - A size, block-sequence or hash mismatch erases the partial image.
   - LED1 turns purple during the download.
   - RTCM streaming continues: the UM980 data is DMA-buffered.
2. **Trial boot:**
   - The board reboots into the new slot as a *try-before-you-buy* image.
   - Within the bootrom's ~16.7 s window, it must load its config and
     detect the W5500. It then calls `rom_explicit_buy`.
   - If it crashes or hangs before that, the bootrom boots the previous
     image.
3. **Health check:**
   - Once bought, the image must reach the network within 10 minutes:
     either download its config or connect to the caster.
   - If it doesn't, it erases its own image header and reboots into the
     previous image.
4. **Rollback bookkeeping:**
   - The previous image notices the failed version and shows "rolled back
     from X" on the status page.
   - It stops retrying a version after 2 failures.

Images carry an embedded SHA-256 hash, checked by the bootrom. They also
carry a version number, which the bootrom uses to pick the newer valid
slot: `VERSION` becomes major = MAJOR and minor = MINOR × 256 + PATCH.

## UM980: receiver firmware

### Download and verification

1. **Staging:** the package is downloaded into a dedicated 4 MB flash area
   at 8 MB, outside the application slots. An RP2350 update can't touch a
   staged package.
2. **Verification** (repeated just before flashing): size, SHA-256, the
   `.pkg` magic, and the `UM980` product name.
3. **Flashing** goes over COM1 at 921600 baud, or 115200 as a fallback.
   NTRIP is paused during the flash and both LEDs turn purple.
4. **After the flash,** the receiver configuration is checked again and
   NTRIP resumes.

Measured downtime: about 75 s.

### The serial bootloader protocol

The sequence is: reset, `T@` trigger, menu `2`, XMODEM-1K-style packets,
`backup succeed`, then menu `6`.

- It follows SparkFun's open-source `UM980_Update_From_Array.ino`, with one
  difference: the receiver pauses for up to ~6 s mid-transfer (flash
  erase), so ACKs are awaited for up to 30 s.
- A transfer aborted mid-way leaves the receiver on its old firmware.

### Maintenance window

- **Format:** `maint_window` is `"HH:MM-HH:MM"` in UTC, and may wrap
  midnight.
- **Scheduled flashing** happens only inside the window, and only with a
  valid GNSS UTC clock.
- **Without a window,** only the button flashes the UM980.

### Failures

- **Receiver back on its old firmware:** the package is never retried
  automatically.
- **Receiver silent afterwards** (e.g. stuck in its bootloader): the station
  retries the flash, even outside the window. It makes up to 3 attempts in
  total, 10 min apart.
- **Reboot mid-upgrade:** attempts are recorded in flash before they start,
  so the station notices one that was interrupted.

### Forced packages

For testing, `"force": true` in the `um980` section allows reinstalling the
same version, or a downgrade.
- Each forced package (file + `id`) is applied **once**.
- To apply it again, publish it with a new `"id"`.

## Security model

- **Integrity:** the SHA-256 in the manifest protects against corrupted
  downloads.
- **Authenticity:** the SHA-256 doesn't protect against someone who controls
  the server.
  - Use `https://` URLs, so manifests and images can't be swapped in
    transit. See [https.md](https.md).
  - The manifest and firmware downloads never fall back to HTTP. Only
    `config.json` does.
- **Signed images:** for authenticated updates, sign the images
  (`pico_sign_binary`) and enable secure boot in the RP2350's OTP. That is
  **irreversible,** and needs a key-management plan first.
