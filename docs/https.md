# HTTPS

Every URL the station fetches (config, telemetry, firmware manifest and
downloads) may be `http://` or `https://`. HTTPS uses Mbed TLS 3.6 from the
Pico SDK, configured in `src/rtk_mbedtls_config.h`.

## Protocol

- **TLS 1.2 client:** ECDHE with ECDSA or RSA certificates, AES-GCM or
  ChaCha20-Poly1305, SNI, and extended master secret.
- **Certificates are always verified,** including the hostname or IP SAN.
- **Certificate dates:** the server certificate and its intermediates are
  checked against the GNSS UTC clock. Without UTC (no PPS), only the date
  check is skipped; the chain and the name are still verified.

## Trust store

The roots live in `third_party/certs/`. You can add your own, e.g. a private
CA, in `provision/ca.pem` (one or more PEM certificates). The bundle is
generated at build time by `tools/gen_ca_bundle.py`.

| Root | Key | Expires | Source, SHA-256 fingerprint |
|---|---|---|---|
| ISRG Root X1 | RSA 4096 | 2035-06-04 | Debian Mozilla bundle, `96:BC:EC:…:08:C6` |
| ISRG Root X2 | ECDSA P-384 | 2040-09-17 | Debian Mozilla bundle, `69:72:9B:…:14:70` |
| Root YR | RSA 4096 | 2045-09-02 | letsencrypt.org, `E5:7B:7E:…:A8:6F` |
| Root YE | ECDSA P-384 | 2045-09-02 | letsencrypt.org, `E1:4F:FC:…:56:66` |

**The "Generation Y" roots:**
- YR and YE are Let's Encrypt's newest roots, issuing since January 2026.
  Their keys were checked against the cross-signatures from X1 and X2.
- Servers currently send chains like
  `leaf ← YE2 ← Root YE ← (cross-sign by X2, expires 2032-09-02)`.
- With YE as an anchor, verification stops at YE, so stations don't depend
  on the cross-signature.

## Root expiry

A station in the field must never be cut off by a root certificate expiring
before it gets the update that replaces it. Two rules guarantee this:

- **The anchors' own dates are not enforced.** They are trusted because they
  are compiled in.
- **The build fails if an anchor expires within 5 years** (`CA_MIN_YEARS`,
  a CMake cache variable).
  - The check runs whenever the trust store is regenerated, which includes
    every `VERSION` bump.
  - When it fails, add the successor roots, and drop the old ones once no
    server needs them.

## HTTP fallback for the config

If `CONFIG_URL` is `https://` and 3 attempts, 2 s apart, all fail without
any HTTP response, the station fetches the same URL once over `http://`.
Typical causes are a handshake or certificate failure, or the server being
down.

**Why:** a station with a stale trust store stays reachable. Its config can
point `firmware_url` to a fixed firmware.

**Details:**
- An explicit port is kept as is; the default port becomes 80.
- Every later download tries HTTPS first again.
- A server that answers over TLS, even with an error such as 404, doesn't
  trigger the fallback.
- `config_source` in `/status.json` shows where the config came from:
  `https`, `http`, `http (HTTPS failed)` or `local` (stored in flash).

**Trade-off:**
- Someone who can block port 443 and intercept port 80 on the path can
  feed the station a config.
- Only `config.json` falls back. The firmware manifest and the firmware
  downloads never do.

## Resource cost

- **Flash:** about 145 KB.
- **Heap:** about 25–40 KB during a connection.
- **Hashing:** downloads use the RP2350's hardware SHA-256; TLS uses
  software SHA.
- **Stack:** TLS handshakes peak at about 4 KB of the 16 KB core 0 stack.

## Tested

| Case | Result |
|---|---|
| Let's Encrypt ECDSA chain | Verified |
| Private RSA CA | A 620 KB OTA image downloaded and installed over HTTPS |
| Self-signed certificate | Rejected: "not correctly signed by the trusted CA" |
| Expired server certificate | Rejected: "validity has expired" |
| Expired private root, valid leaf | Accepted, since anchor dates are not enforced; the build refused that root until `CA_MIN_YEARS` was overridden |
| Config fallback | An expired leaf was rejected 3 times, then the config was fetched over `http://`. A 404 over TLS did not fall back. A working certificate brought `config_source` back to `https`. |
