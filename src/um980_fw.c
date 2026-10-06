#include "um980_fw.h"

#include <stdio.h>
#include <string.h>

#include "app_status.h"
#include "board.h"
#include "config.h"
#include "flash_store.h"
#include "fw_manifest.h"
#include "gnss_io.h"
#include "gnss_logic.h"
#include "http_client.h"
#include "idle.h"
#include "json_util.h"
#include "led.h"
#include "net.h"
#include "ota.h"
#include "ntrip.h"
#include "pico/sha256.h"
#include "pico/time.h"
#include "um980.h"
#include "utc.h"
#include "util.h"

#define STAGE_TAG            0x47465334u  // "GFS4"
#define DONE_INSTALLED 1
#define DONE_FAILED    2
#define ACK_TIMEOUT_MS       30000u
#define MAX_ATTEMPTS         3
#define RESCUE_SPACING_MS    (10u * 60u * 1000u)
#define FIRST_CHECK_MS       (3u * 60u * 1000u)
#define RETRY_CHECK_MS       (30u * 60u * 1000u)
#define DOWNLOAD_TIMEOUT_MS  (10u * 60u * 1000u)
#define FAST_BAUD            921600u
#define NORMAL_BAUD          UM980_BAUDRATE
#define PKT_DATA             1024u
#define PKT_SIZE             (3u + PKT_DATA + 1u)
#define XM_STX 0x02
#define XM_EOT 0x04
#define XM_ACK 0x06
#define XM_NAK 0x15
#define XM_CAN 0x18

typedef struct {
    uint32_t tag;
    // Staged package (in GNSS_STAGE_OFFSET).
    char version[32];
    uint8_t sha[32];
    uint8_t key[32];   // sha mixed with the manifest "id": what installed/failed track
    uint32_t size;
    bool force;
    // Outcome bookkeeping, by package hash.
    // Recent outcomes, by package key (ring). A forced package acts once;
    // a failed one is never retried automatically.
    uint8_t done_key[8][32];
    uint8_t done_kind[8];  // 0 empty, 1 installed, 2 failed
    uint8_t done_next;
    // Attempts at the staged package. Saved *before* each attempt, so a
    // reboot in the middle is noticed.
    uint32_t attempts;
    bool in_progress;
} stage_rec_t;

static stage_rec_t rec;
static bool busy;
static bool check_requested;
static absolute_time_t next_check;
static absolute_time_t next_rescue;
static char state_text[96] = "idle";

static void set_state(const char *fmt, const char *arg) {
    snprintf(state_text, sizeof(state_text), fmt, arg);
}

const char *gnss_fw_state(void) {
    return state_text;
}

bool gnss_fw_busy(void) {
    return busy;
}

// "Check for new firmware" button: check now, flash a newer package now
// (not in the maintenance window: whoever pressed it accepts the downtime),
// then let the RP2350 OTA check run. The RP2350 update reboots the device, so
// it comes after the UM980 job, never during it.
static bool flash_now;

void gnss_fw_request_check(void) {
    check_requested = true;
    flash_now = true;
    fw_manifest_invalidate();  // a release published seconds ago must be seen
}

// The button's UM980 part is over (flashed, nothing to do, or impossible):
// hand over to the RP2350 OTA check.
static void button_done(void) {
    if (!flash_now) return;
    flash_now = false;
    ota_request_check();
}

static void rec_save(void) {
    rec.tag = STAGE_TAG;
    if (!blob_save(BLOB_GNSS, &rec, sizeof(rec))) printf("GNSS FW: failed to save state\n");
}

void gnss_fw_init(void) {
    memset(&rec, 0, sizeof(rec));
    stage_rec_t tmp;
    if (blob_load(BLOB_GNSS, &tmp, sizeof(tmp)) == (int)sizeof(tmp) && tmp.tag == STAGE_TAG) {
        rec = tmp;
        rec.version[sizeof(rec.version) - 1] = '\0';
    }
    next_check = make_timeout_time_ms(FIRST_CHECK_MS);
    next_rescue = get_absolute_time();
    if (rec.in_progress) {
        // The RP2350 restarted during an attempt: the outcome is checked
        // against the receiver on the next poll.
        printf("⚠ GNSS FW: an upgrade to %s was interrupted (attempt %lu)\n", rec.version, (unsigned long)rec.attempts);
        rec.in_progress = false;
        rec_save();
    }
    if (rec.size) set_state("staged %s", rec.version);
}

static bool sha_eq(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 32) == 0;
}

static int done_lookup(const uint8_t key[32]) {
    for (int i = 0; i < 8; i++)
        if (rec.done_kind[i] && sha_eq(rec.done_key[i], key)) return rec.done_kind[i];
    return 0;
}

static void done_remember(const uint8_t key[32], uint8_t kind) {
    for (int i = 0; i < 8; i++) {
        if (rec.done_kind[i] && sha_eq(rec.done_key[i], key)) {
            rec.done_kind[i] = kind;
            return;
        }
    }
    memcpy(rec.done_key[rec.done_next], key, 32);
    rec.done_kind[rec.done_next] = kind;
    rec.done_next = (uint8_t)((rec.done_next + 1) % 8);
}

// Whether the staged/offered package must not be (re)applied.
static bool blocked(const uint8_t key[32], bool force) {
    int d = done_lookup(key);
    return d == DONE_FAILED || (force && d == DONE_INSTALLED);
}

// Package identity for the installed/failed bookkeeping: the file hash,
// mixed with the manifest's optional "id" so an operator can deliberately
// retry a package by publishing it with a new id.
static void make_key(const uint8_t sha[32], const char *id, uint8_t key[32]) {
    memcpy(key, sha, 32);
    uint32_t h = 2166136261u;
    for (; id && *id; id++) h = (h ^ (uint8_t)*id) * 16777619u;
    if (h != 2166136261u)
        for (int i = 0; i < 4; i++) key[i] ^= (uint8_t)(h >> (8 * i));
}

// --- Staging -----------------------------------------------------------------------

typedef struct {
    uint8_t sector[FLASH_SECTOR];
    size_t fill;
    uint32_t written;
    size_t total;
    long expected;
    pico_sha256_state_t sha;
    bool sha_started;
    bool header_ok;
    char error[40];
} stage_ctx_t;

static stage_ctx_t sc;

static bool stage_fail(const char *msg) {
    str_copy(sc.error, sizeof(sc.error), msg);
    return false;
}

static bool stage_flush(void) {
    if (!sc.fill) return true;
    size_t len = (sc.fill + FLASH_PAGE - 1) & ~(FLASH_PAGE - 1);
    memset(sc.sector + sc.fill, 0xFF, len - sc.fill);
    uint32_t off = GNSS_STAGE_OFFSET + sc.written;
    if (!flash_erase_sector(off) || !flash_program(off, sc.sector, len)) return stage_fail("flash write failed");
    sc.written += (uint32_t)sc.fill;
    sc.fill = 0;
    return true;
}

static bool stage_body(void *ctx, const uint8_t *data, size_t len) {
    if (!sc.sha_started) {
        if (pico_sha256_start_blocking(&sc.sha, SHA256_BIG_ENDIAN, false) != PICO_OK) return stage_fail("SHA busy");
        sc.sha_started = true;
    }
    pico_sha256_update_blocking(&sc.sha, data, len);
    sc.total += len;
    if (sc.total > GNSS_STAGE_SIZE || (sc.expected > 0 && sc.total > (size_t)sc.expected))
        return stage_fail("package too large");
    while (len) {
        size_t take = FLASH_SECTOR - sc.fill;
        if (take > len) take = len;
        memcpy(sc.sector + sc.fill, data, take);
        sc.fill += take;
        data += take;
        len -= take;
        if (!sc.header_ok && sc.written == 0 && sc.fill >= UM980_PKG_HEADER_SIZE) {
            if (!um980_pkg_header_ok(sc.sector, sc.fill, "UM980")) return stage_fail("not a UM980 package");
            sc.header_ok = true;
        }
        if (sc.fill == FLASH_SECTOR && !stage_flush()) return false;
    }
    return true;
}

static bool stage_package(const char *version, const char *url, long size, const uint8_t sha[32], const uint8_t key[32],
                          bool force) {
    memset(&sc, 0, sizeof(sc));
    sc.expected = size;
    printf("\n=== GNSS FW: downloading %s ===\n%s\n", version, url);
    set_state("downloading %s", version);

    http_result_t res;
    bool ok = http_request("GET", url, NULL, NULL, 0, stage_body, NULL, &res, DOWNLOAD_TIMEOUT_MS);
    sha256_result_t digest;
    if (sc.sha_started) pico_sha256_finish(&sc.sha, &digest);
    if (ok && res.status != 200) {
        snprintf(sc.error, sizeof(sc.error), "HTTP %d", res.status);
        ok = false;
    }
    if (!ok && !sc.error[0]) str_copy(sc.error, sizeof(sc.error), "connection or TLS failed");
    if (ok) ok = stage_flush();
    if (ok && !sc.header_ok) ok = stage_fail("not a UM980 package");
    if (ok && size > 0 && sc.total != (size_t)size) ok = stage_fail("size mismatch");
    if (ok && !sha_eq(digest.bytes, sha)) ok = stage_fail("SHA-256 mismatch");
    if (!ok) {
        printf("✗ GNSS FW: %s\n", sc.error);
        set_state("download failed: %s", sc.error);
        rec.size = 0;
        rec_save();
        return false;
    }
    str_copy(rec.version, sizeof(rec.version), version);
    memcpy(rec.sha, sha, 32);
    memcpy(rec.key, key, 32);
    rec.size = (uint32_t)sc.total;
    rec.force = force;
    rec.attempts = 0;
    rec.in_progress = false;
    rec_save();
    printf("✓ GNSS FW: %s staged (%u bytes)\n", version, (unsigned)sc.total);
    set_state("staged %s", version);
    return true;
}

// Re-hash the staged copy from flash before handing it to the receiver.
static bool staged_copy_ok(void) {
    static uint8_t buf[FLASH_SECTOR];
    pico_sha256_state_t st;
    if (pico_sha256_start_blocking(&st, SHA256_BIG_ENDIAN, false) != PICO_OK) return false;
    for (uint32_t off = 0; off < rec.size; off += FLASH_SECTOR) {
        uint32_t n = rec.size - off < FLASH_SECTOR ? rec.size - off : FLASH_SECTOR;
        if (!flash_read(GNSS_STAGE_OFFSET + off, buf, FLASH_SECTOR)) {
            pico_sha256_finish(&st, (sha256_result_t *)buf);
            return false;
        }
        if (off == 0 && !um980_pkg_header_ok(buf, n, "UM980")) {
            pico_sha256_finish(&st, (sha256_result_t *)buf);
            return false;
        }
        pico_sha256_update_blocking(&st, buf, n);
        idle_poll();
    }
    sha256_result_t d;
    pico_sha256_finish(&st, &d);
    return sha_eq(d.bytes, rec.sha);
}

// --- Bootloader ----------------------------------------------------------------------

static const char *boot_error;

static bool fail(const char *e) {
    boot_error = e;
    printf("✗ GNSS FW: %s\n", e);
    return false;
}

// Switch COM1 and our UART to `baud`; returns true if the receiver answers.
static bool switch_baud(uint32_t baud) {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "CONFIG COM1 %lu\r\n", (unsigned long)baud);
    um980_ctrl_discard_input();
    um980_ctrl_write(cmd, strlen(cmd));
    idle_sleep_ms(200);
    um980_ctrl_set_baud(baud);
    idle_sleep_ms(100);
    return um980_query("VERSIONA", 1500) != NULL;
}

static bool enter_bootloader(void) {
    um980_ctrl_discard_input();
    if (!gnss_reset_pulse(500)) return fail("reset line unavailable");
    // Spam the trigger until the bootloader menu shows up.
    static const char trigger[] = "T@T@T@T@T@T@T@T@";
    absolute_time_t deadline = make_timeout_time_ms(10000);
    char win[16];
    const char *needle = "efuse from uart";
    size_t n = strlen(needle), have = 0;
    absolute_time_t next_send = get_absolute_time();
    while (!time_reached(deadline)) {
        if (time_reached(next_send)) {
            um980_ctrl_write(trigger, sizeof(trigger) - 1);
            next_send = make_timeout_time_ms(20);
        }
        int c = um980_ctrl_read_byte(2);
        if (c < 0) continue;
        if (have == n) {
            memmove(win, win + 1, n - 1);
            have--;
        }
        win[have++] = (char)c;
        if (have == n && memcmp(win, needle, n) == 0) return true;
    }
    return fail("bootloader menu not reached");
}

static bool send_packets(void) {
    static uint8_t pkt[PKT_SIZE];
    static const uint8_t replies[] = {XM_ACK, XM_NAK, XM_CAN};
    uint8_t seq = 1;
    uint32_t sent = 0, packets = 0;
    int last_pct = -1;
    while (sent < rec.size) {
        uint32_t n = rec.size - sent < PKT_DATA ? rec.size - sent : PKT_DATA;
        pkt[0] = XM_STX;
        pkt[1] = seq;
        pkt[2] = (uint8_t)(0xFF - seq);
        if (!flash_read(GNSS_STAGE_OFFSET + sent, pkt + 3, PKT_DATA)) return fail("flash read failed");
        if (n < PKT_DATA) {
            pkt[3 + n] = 0x1A;  // EOF marker, then zero padding
            memset(pkt + 3 + n + 1, 0, PKT_DATA - n - 1);
        }
        uint8_t sum = 0;
        for (unsigned i = 0; i < PKT_SIZE - 1; i++) sum += pkt[i];
        pkt[PKT_SIZE - 1] = (uint8_t)(sum - 1);

        int naks = 0, timeouts = 0;
        for (;;) {
            um980_ctrl_discard_input();  // only a reply to this packet counts
            um980_ctrl_write(pkt, PKT_SIZE);
            absolute_time_t t_sent = get_absolute_time();
            // The receiver can pause for seconds (flash erase): wait patiently
            // rather than resending into a busy receiver.
            int r = um980_ctrl_wait_byte(replies, sizeof(replies), ACK_TIMEOUT_MS);
            int64_t ms = absolute_time_diff_us(t_sent, get_absolute_time()) / 1000;
            if (ms > 2000) printf("GNSS FW: packet %lu reply after %lld ms\n", (unsigned long)packets, ms);
            if (r == XM_ACK) break;
            if (r == XM_CAN) return fail("transfer cancelled by receiver");
            if (r < 0 && ++timeouts > 1) return fail("no ACK from receiver");
            if (r == XM_NAK && ++naks > 3) return fail("packet rejected 3 times");
            printf("GNSS FW: packet %lu %s, resending\n", (unsigned long)packets, r < 0 ? "timeout" : "NAK");
        }
        sent += n;
        packets++;
        seq++;
        int pct = (int)((uint64_t)sent * 100 / rec.size);
        if (pct / 10 != last_pct / 10) {
            last_pct = pct;
            char p[8];
            snprintf(p, sizeof(p), "%d%%", pct);
            set_state("flashing UM980 %s", p);
            printf("GNSS FW: %s\n", p);
        }
    }
    printf("GNSS FW: %lu packets sent\n", (unsigned long)packets);
    return true;
}

// One complete bootloader session. Leaves COM1 and our UART at 115200.
static bool bootload(uint32_t baud) {
    boot_error = NULL;
    printf("\n=== GNSS FW: flashing %s at %lu baud ===\n", rec.version, (unsigned long)baud);
    bool ok = enter_bootloader();
    if (ok) {
        printf("GNSS FW: bootloader menu reached\n");
        um980_ctrl_write("\r\n2\r\n", 5);
        if (!um980_ctrl_wait_string("download to", 10000)) ok = fail("download mode not entered");
    }
    if (ok) {
        static const uint8_t nak = XM_NAK;
        if (um980_ctrl_wait_byte(&nak, 1, 10000) < 0) ok = fail("receiver not ready (no 0x15)");
        idle_sleep_ms(50);
        um980_ctrl_discard_input();  // drop further "ready" polls
    }
    if (ok) ok = send_packets();
    if (ok) {
        static const uint8_t eot = XM_EOT;
        um980_ctrl_discard_input();
        um980_ctrl_write(&eot, 1);
        printf("GNSS FW: transfer complete, receiver verifying (up to 90 s)...\n");
        set_state("UM980 verifying %s", rec.version);
        if (!um980_ctrl_wait_string("backup succeed", 90000)) ok = fail("no 'backup succeed'");
    }
    // Leave the bootloader in every case.
    if (!um980_ctrl_wait_string("efuse from uart", ok ? 10000 : 1000) && ok)
        printf("GNSS FW: menu not reprinted, exiting anyway\n");
    um980_ctrl_write("6\r\n6\r\n6\r\n", 9);
    bool reset_seen = um980_ctrl_wait_string("resetting the cpu", 10000);
    um980_ctrl_set_baud(NORMAL_BAUD);
    if (!reset_seen) gnss_reset_pulse(500);  // make sure it restarts
    um980_ctrl_wait_string("$devicename,COM", 8000);
    idle_sleep_ms(1000);
    return ok;
}

// Detect the receiver; a freshly flashed UM980 may take a while to boot.
static bool detect(char *fw, size_t fw_size) {
    char model[sizeof(app_status.um980_model)];
    for (int i = 0; i < 2; i++) {
        if (um980_start(model, sizeof(model), fw, fw_size)) {
            str_copy(app_status.um980_model, sizeof(app_status.um980_model), model);
            str_copy(app_status.um980_firmware, sizeof(app_status.um980_firmware), fw);
            return true;
        }
        idle_sleep_ms(10000);
    }
    return false;
}

// Returns whether the receiver is up afterwards.
static bool run_upgrade(bool rescue) {
    printf("\n=== GNSS FW: %s UM980 %s -> %s (attempt %lu/%d) ===\n", rescue ? "rescuing" : "upgrading",
           app_status.um980_firmware, rec.version, (unsigned long)rec.attempts + 1, MAX_ATTEMPTS);
    set_state("verifying staged %s", rec.version);
    if (!staged_copy_ok()) {
        printf("✗ GNSS FW: staged copy corrupt, discarding\n");
        set_state("staged copy corrupt", NULL);
        rec.size = 0;
        rec_save();
        return !rescue;
    }

    rec.attempts++;
    rec.in_progress = true;
    rec_save();

    busy = true;
    if (!ntrip_pause(30000)) printf("⚠ GNSS FW: NTRIP did not pause cleanly\n");
    led_both(COLOR_PURPLE);
    absolute_time_t t0 = get_absolute_time();

    // Fast transfer if the receiver accepts it, else the normal rate. In
    // rescue mode the receiver doesn't answer: go straight to 115200.
    uint32_t baud = !rescue && switch_baud(FAST_BAUD) ? FAST_BAUD : NORMAL_BAUD;
    if (!rescue && baud != FAST_BAUD) switch_baud(NORMAL_BAUD);
    bool ok = bootload(baud);
    if (!ok && baud == FAST_BAUD) {
        printf("GNSS FW: retrying at %u baud\n", NORMAL_BAUD);
        ok = bootload(NORMAL_BAUD);
    }

    char fw[sizeof(app_status.um980_firmware)] = "";
    bool up = detect(fw, sizeof(fw));
    bool version_ok = ok && up && um980_build_number(fw) == um980_build_number(rec.version);
    uint32_t secs = (uint32_t)(absolute_time_diff_us(t0, get_absolute_time()) / 1000000);
    rec.in_progress = false;

    if (version_ok) {
        printf("✓ GNSS FW: UM980 now runs %s (%lu s offline)\n", fw, (unsigned long)secs);
        done_remember(rec.key, DONE_INSTALLED);
        rec.attempts = 0;
        set_state("installed %s", rec.version);
    } else if (up) {
        // Back on its old firmware: safe, don't insist on this package.
        printf("✗ GNSS FW: upgrade failed (%s), receiver back on %s\n", boot_error ? boot_error : "version not confirmed", fw);
        done_remember(rec.key, DONE_FAILED);
        set_state("failed: %s", boot_error ? boot_error : "version not confirmed");
    } else if (rec.attempts >= MAX_ATTEMPTS) {
        printf("✗ GNSS FW: receiver DOWN after %d attempts, giving up\n", MAX_ATTEMPTS);
        done_remember(rec.key, DONE_FAILED);
        set_state("receiver down, gave up (%s)", boot_error ? boot_error : "no answer");
    } else {
        printf("✗ GNSS FW: receiver DOWN (%s), will retry in 10 min\n", boot_error ? boot_error : "no answer");
        set_state("receiver down, retrying (%s)", boot_error ? boot_error : "no answer");
        next_rescue = make_timeout_time_ms(RESCUE_SPACING_MS);
    }
    rec_save();
    status_restore_leds();
    ntrip_resume();
    busy = false;
    return up;
}

// --- Manifest ----------------------------------------------------------------------

static bool hex_to_sha(const char *hex, uint8_t out[32]) {
    if (strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

static struct {
    bool active;
    absolute_time_t at;
    char version[32], url[CFG_URL];
    long size;
    uint8_t sha[32], key[32];
    bool force;
} pending;

static void check_manifest(bool manual) {
    // The "um980" section of the shared firmware manifest (fw_manifest.h).
    static json_doc_t doc;
    switch (fw_manifest_section("um980", &doc)) {
    case FWM_OK:
        break;
    case FWM_UNAVAILABLE:
        set_state("manifest unavailable", NULL);
        return;
    case FWM_ABSENT:  // no UM980 release published
        set_state("up to date (%s)", app_status.um980_firmware);
        return;
    case FWM_DISABLED:
        return;
    default:
        set_state("invalid manifest", NULL);
        return;
    }
    char model[16] = "", version[32] = "", url[CFG_URL] = "", sha_hex[72] = "", id[48] = "";
    double size = -1;
    bool force = false;
    int t;
    if ((t = json_find(&doc, "model")) >= 0) json_tok_str(&doc, t, model, sizeof(model));
    if ((t = json_find(&doc, "version")) >= 0) json_tok_str(&doc, t, version, sizeof(version));
    if ((t = json_find(&doc, "url")) >= 0) json_tok_str(&doc, t, url, sizeof(url));
    if ((t = json_find(&doc, "sha256")) >= 0) json_tok_str(&doc, t, sha_hex, sizeof(sha_hex));
    if ((t = json_find(&doc, "size")) >= 0) json_tok_double(&doc, t, &size);
    if ((t = json_find(&doc, "force")) >= 0) json_tok_bool(&doc, t, &force);
    if ((t = json_find(&doc, "id")) >= 0) json_tok_str(&doc, t, id, sizeof(id));
    uint8_t sha[32];
    if (strcmp(model, "UM980") != 0 || um980_build_number(version) < 0 || !url[0] || !hex_to_sha(sha_hex, sha)) {
        printf("GNSS FW: manifest needs model UM980, version, url and sha256\n");
        set_state("invalid manifest", NULL);
        return;
    }

    uint8_t key[32];
    make_key(sha, id, key);
    long want = um980_build_number(version), have = um980_build_number(app_status.um980_firmware);
    if (force && done_lookup(key) == DONE_INSTALLED) {
        printf("GNSS FW: forced %s already applied\n", version);
        set_state("up to date (%s)", app_status.um980_firmware);
        return;
    }
    if (done_lookup(key) == DONE_FAILED) {
        printf("GNSS FW: %s failed before, not retrying\n", version);
        set_state("skipping failed %s", version);
        return;
    }
    if (!force && have >= 0 && want <= have) {
        printf("GNSS FW: up to date (%s, manifest %s)\n", app_status.um980_firmware, version);
        set_state("up to date (%s)", app_status.um980_firmware);
        return;
    }
    if (rec.size && sha_eq(sha, rec.sha)) {
        // Same file already staged: adopt the manifest's id/force.
        if (rec.force != force || !sha_eq(key, rec.key)) {
            rec.force = force;
            memcpy(rec.key, key, 32);
            rec.attempts = 0;
            rec_save();
        }
        set_state("staged %s, waiting for window", version);
        return;
    }
    if (!manual && pending.active && sha_eq(pending.key, key)) return;  // already scheduled
    uint32_t delay = manual ? 0 : random_delay_ms((uint32_t)(config.download_jitter_s > 0 ? config.download_jitter_s : 0));
    if (!delay) {
        pending.active = false;
        stage_package(version, url, (long)size, sha, key, force);
        return;
    }
    // Scheduled check: spread the fleet's downloads.
    pending.active = true;
    pending.at = make_timeout_time_ms(delay);
    str_copy(pending.version, sizeof(pending.version), version);
    str_copy(pending.url, sizeof(pending.url), url);
    pending.size = (long)size;
    memcpy(pending.sha, sha, 32);
    memcpy(pending.key, key, 32);
    pending.force = force;
    printf("GNSS FW: %s available, download in %lu s (jitter)\n", version, (unsigned long)(delay / 1000));
    set_state("download %s pending (jitter)", version);
}

static bool in_window(void) {
    int64_t ms;
    if (!maint_window_valid(config.maint_window) || !utc_now_ms(&ms)) return false;
    int minute = (int)((ms / 60000) % 1440);
    return maint_window_contains(config.maint_window, minute);
}

void gnss_fw_poll(bool *um980_ok) {
    if (busy) return;
    if (pending.active && time_reached(pending.at) && net_is_up() && !check_requested) {
        pending.active = false;
        stage_package(pending.version, pending.url, pending.size, pending.sha, pending.key, pending.force);
    }
    if (net_is_up() && (check_requested || time_reached(next_check))) {
        // The button and the first check after boot are immediate; later
        // scheduled checks get the random download delay.
        static bool boot_check = true;
        bool manual = check_requested || boot_check;
        boot_check = false;
        check_requested = false;
        if (manual) pending.active = false;
        // Pick up remote config changes (firmware_url, maint_window, ...)
        // without a reboot.
        config_download();
        if (!config.firmware_url[0]) {
            next_check = make_timeout_time_ms((uint32_t)(config.ota_interval_h > 0 ? config.ota_interval_h : 6) * 3600000u);
            button_done();
            return;
        }
        check_manifest(manual);
        uint32_t hours = config.ota_interval_h > 0 && config.ota_interval_h < 24 * 7 ? (uint32_t)config.ota_interval_h : 6;
        uint32_t ms = strstr(state_text, "unavailable") || strstr(state_text, "failed") ? RETRY_CHECK_MS
                                                                                         : hours * 3600u * 1000u;
        next_check = make_timeout_time_ms(ms);
    }

    if (flash_now && check_requested) return;  // button pressed, its check hasn't run yet (network down)
    // After boot the state reads "staged X" until the first manifest check
    // (3 min): say "up to date" when X is what the receiver already runs.
    if (rec.size && !rec.force && str_starts_with(state_text, "staged") && app_status.um980_firmware[0] &&
        um980_build_number(rec.version) <= um980_build_number(app_status.um980_firmware))
        set_state("up to date (%s)", app_status.um980_firmware);
    if (!rec.size || !gnss_reset_available() || blocked(rec.key, rec.force)) {
        button_done();
        return;
    }

    // Rescue: an earlier attempt at this package left the receiver silent
    // (stuck in its bootloader). The station is down anyway, so don't wait
    // for the window; space attempts and stop after MAX_ATTEMPTS.
    if (!*um980_ok) {
        if (rec.attempts > 0 && rec.attempts < MAX_ATTEMPTS && time_reached(next_rescue)) *um980_ok = run_upgrade(true);
        button_done();
        return;
    }
    // Interrupted attempt that actually completed.
    if (rec.attempts > 0 && um980_build_number(app_status.um980_firmware) == um980_build_number(rec.version)) {
        done_remember(rec.key, DONE_INSTALLED);
        rec.attempts = 0;
        rec_save();
        set_state("installed %s", rec.version);
        button_done();
        return;
    }
    if (rec.attempts >= MAX_ATTEMPTS) {
        done_remember(rec.key, DONE_FAILED);
        rec_save();
        set_state("failed after %s attempts", "3");
        button_done();
        return;
    }
    long want = um980_build_number(rec.version), have = um980_build_number(app_status.um980_firmware);
    if (!rec.force && have >= 0 && want <= have) {
        button_done();
        return;
    }
    if (!flash_now && !in_window()) {
        if (!strstr(state_text, "window")) set_state("staged %s, waiting for window", rec.version);
        return;
    }
    if (flash_now) printf("GNSS FW: flashing now (button), outside the maintenance window if need be\n");
    *um980_ok = run_upgrade(false);
    button_done();
}
