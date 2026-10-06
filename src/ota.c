#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "app_status.h"
#include "boot/picobin.h"
#include "boot/picoboot_constants.h"
#include "boot/uf2.h"
#include "config.h"
#include "flash_store.h"
#include "hardware/regs/addressmap.h"
#include "http_client.h"
#include "idle.h"
#include "json_util.h"
#include "led.h"
#include "fw_manifest.h"
#include "net.h"
#include "ntrip.h"
#include "pico/bootrom.h"
#include "pico/flash.h"
#include "pico/sha256.h"
#include "pico/time.h"
#include "util.h"

#define FIRST_CHECK_DELAY_MS 60000u
#define RETRY_CHECK_DELAY_MS (30u * 60u * 1000u)
#define DOWNLOAD_TIMEOUT_MS  (5u * 60u * 1000u)
#define OTA_STATE_TAG        0x4f544132u  // "OTA2"
#define MAX_BOOT_ATTEMPTS    3

typedef struct {
    uint32_t tag;
    char pending[16];   // version we rebooted into, until it proves healthy
    char failed[16];    // last version that failed
    uint32_t fail_count;
    uint32_t boot_attempts;  // boots of `pending` that didn't become healthy
} ota_state_t;

static ota_state_t state;
static bool trial_boot;      // bootrom says a buy is pending
static bool fresh_update;    // we are the pending version, not yet healthy
static bool healthy;
static absolute_time_t next_check;
static volatile bool check_requested;
static uint8_t workarea[4096] __attribute__((aligned(4)));

static void set_state_text(const char *fmt, const char *arg) {
    snprintf(app_status.ota_state, sizeof(app_status.ota_state), fmt, arg);
}

static void state_load(void) {
    memset(&state, 0, sizeof(state));
    ota_state_t tmp;
    if (blob_load(BLOB_OTA, &tmp, sizeof(tmp)) == (int)sizeof(tmp) && tmp.tag == OTA_STATE_TAG) {
        state = tmp;
        state.pending[sizeof(state.pending) - 1] = '\0';
        state.failed[sizeof(state.failed) - 1] = '\0';
    }
    state.tag = OTA_STATE_TAG;
}

static void state_save(void) {
    if (!blob_save(BLOB_OTA, &state, sizeof(state))) printf("OTA: failed to save state\n");
}

// Flash location (offset, size) of partition idx from the partition table.
static bool partition_range(int idx, uint32_t *offset, uint32_t *size) {
    uint32_t buf[3];
    int rc = rom_get_partition_table_info(buf, 3, PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION |
                                                      ((uint32_t)idx << 24));
    if (rc != 3) return false;
    uint32_t loc = buf[1];
    uint32_t first = (loc & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
    uint32_t last = (loc & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
    *offset = first * FLASH_SECTOR;
    *size = (last + 1 - first) * FLASH_SECTOR;
    return true;
}

static void revert_to_previous(const char *why);

void ota_boot_init(void) {
    boot_info_t info;
    if (rom_get_boot_info(&info)) {
        app_status.boot_partition = info.partition;
        trial_boot = (info.tbyb_and_update_info & BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING) != 0;
        printf("Boot partition: %d, boot type: %d%s\n", info.partition, info.boot_type,
               trial_boot ? " (trial image, buy pending)" : "");
    } else {
        printf("Boot info unavailable (no partition table?)\n");
    }

    state_load();
    set_state_text("idle", NULL);
    if (state.pending[0]) {
        if (strcmp(state.pending, FW_VERSION_STRING) == 0) {
            fresh_update = true;
            state.boot_attempts++;
            state_save();
            printf("OTA: running freshly updated version %s, verifying (boot %lu/%d)\n", FW_VERSION_STRING,
                   (unsigned long)state.boot_attempts, MAX_BOOT_ATTEMPTS);
            set_state_text("verifying %s", FW_VERSION_STRING);
            // Resetting before the health check passes (crash, hang,
            // watchdog) restarts the 10-minute timer, so count boots too.
            if (state.boot_attempts > MAX_BOOT_ATTEMPTS) revert_to_previous("kept resetting");
        } else {
            // We rebooted into `pending` but are running something else: it
            // was rejected by the bootrom or reverted itself.
            printf("⚠ OTA: update to %s failed, rolled back to %s\n", state.pending, FW_VERSION_STRING);
            if (strcmp(state.failed, state.pending) == 0) state.fail_count++;
            else {
                str_copy(state.failed, sizeof(state.failed), state.pending);
                state.fail_count = 1;
            }
            set_state_text("rolled back from %s", state.pending);
            state.pending[0] = '\0';
            state.boot_attempts = 0;
            state_save();
        }
    }
    next_check = make_timeout_time_ms(FIRST_CHECK_DELAY_MS);
}

void ota_buy(void) {
    if (!trial_boot) return;
    // The SDK wrapper runs the buy inside flash_safe_execute itself.
    int rc = rom_explicit_buy(workarea, sizeof(workarea));
    if (rc == 0) {
        printf("✓ OTA: image bought (stage 1 passed)\n");
        trial_boot = false;
    } else {
        printf("✗ OTA: explicit buy failed (%d)\n", rc);
    }
}

bool ota_buy_pending(void) {
    return trial_boot;
}

void ota_mark_healthy(void) {
    if (healthy) return;
    healthy = true;
    if (fresh_update) {
        printf("✓ OTA: version %s is healthy (stage 2 passed)\n", FW_VERSION_STRING);
        fresh_update = false;
        state.pending[0] = '\0';
        state.boot_attempts = 0;
        if (strcmp(state.failed, FW_VERSION_STRING) == 0) {
            state.failed[0] = '\0';
            state.fail_count = 0;
        }
        state_save();
        set_state_text("updated to %s", FW_VERSION_STRING);
    }
}

// --- Stage 2 revert ----------------------------------------------------------

typedef struct {
    rom_flash_op_fn flash_op;
    rom_reboot_fn reboot;
    uint32_t offset;
    int rc;
} revert_args_t;

// Runs from RAM with core 1 parked and IRQs off: erases the first sector of
// our own image (its header and vector table) so the bootrom can only pick
// the previous image, then reboots without returning to flash code.
static void __no_inline_not_in_flash_func(revert_job)(void *arg) {
    revert_args_t *a = arg;
    cflash_flags_t f = {.flags = (CFLASH_OP_VALUE_ERASE << CFLASH_OP_LSB) |
                                 (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB) |
                                 (CFLASH_ASPACE_VALUE_STORAGE << CFLASH_ASPACE_LSB)};
    a->rc = a->flash_op(f, XIP_BASE + a->offset, FLASH_SECTOR, NULL);
    if (a->rc != 0) return;  // image intact: keep running
    a->reboot(REBOOT2_FLAG_REBOOT_TYPE_NORMAL, 10, 0, 0);
    for (;;) __asm volatile("wfi");
}

static void revert_to_previous(const char *why) {
    uint32_t off, size;
    if (app_status.boot_partition < 0 || !partition_range(app_status.boot_partition, &off, &size)) {
        printf("✗ OTA: cannot locate own partition, not reverting\n");
        return;
    }
    printf("✗ OTA: version %s %s, reverting to the previous image\n", FW_VERSION_STRING, why);
    // NTRIP keeps running: if the revert fails we carry on as before, and if
    // it succeeds the reboot drops the caster connection anyway.
    static revert_args_t args;
    args.flash_op = (rom_flash_op_fn)rom_func_lookup(ROM_FUNC_FLASH_OP);
    args.reboot = (rom_reboot_fn)rom_func_lookup(ROM_FUNC_REBOOT);
    args.offset = off;
    args.rc = -1;
    // Calling the raw ROM function requires owning the bootrom flash lock.
    if (!bootrom_try_acquire_lock(BOOTROM_LOCK_FLASH_OP)) {
        printf("✗ OTA: flash lock busy, revert postponed\n");
        return;
    }
    int rc = flash_safe_execute(revert_job, &args, 1000);
    bootrom_release_lock(BOOTROM_LOCK_FLASH_OP);
    printf("✗ OTA: revert failed (safe_execute %d, erase %d)\n", rc, args.rc);
}

// --- Download ----------------------------------------------------------------

typedef struct {
    uint8_t block[512];
    size_t fill;
    pico_sha256_state_t sha;
    bool sha_started;
    uint32_t part_offset, part_size;
    bool have_target;
    uint32_t num_blocks, blocks_done;
    uint8_t erased[512 / 8 * 2];  // one bit per 4K sector, up to 4 MiB
    bool wrote_any;
    size_t total;
    long expected_size;
    char error[32];
} dl_ctx_t;

static dl_ctx_t dl;

static bool dl_fail(const char *msg) {
    str_copy(dl.error, sizeof(dl.error), msg);
    return false;
}

static bool target_partition(void) {
    resident_partition_t part;
    int rc;
    // rom_get_uf2_target_partition picks the A/B partition a UF2 of this
    // family would be written to, which is the one we're not running from.
    rc = rom_get_uf2_target_partition(workarea, sizeof(workarea), RP2350_ARM_S_FAMILY_ID, &part);
    if (rc < 0) return false;
    uint32_t first = (part.permissions_and_location & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >>
                     PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
    uint32_t last = (part.permissions_and_location & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >>
                    PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
    dl.part_offset = first * FLASH_SECTOR;
    dl.part_size = (last + 1 - first) * FLASH_SECTOR;

    uint32_t own_off, own_size;
    if (app_status.boot_partition >= 0 && partition_range(app_status.boot_partition, &own_off, &own_size) &&
        own_off == dl.part_offset) {
        return false;  // would overwrite the running image
    }
    printf("OTA: target partition at 0x%06lx, %lu KB\n", (unsigned long)dl.part_offset,
           (unsigned long)(dl.part_size / 1024));
    return dl.part_size <= sizeof(dl.erased) * 8 * FLASH_SECTOR;
}

static bool handle_block(const struct uf2_block *b) {
    if (b->magic_start0 != UF2_MAGIC_START0 || b->magic_start1 != UF2_MAGIC_START1 || b->magic_end != UF2_MAGIC_END)
        return dl_fail("bad UF2 block");
    if (dl.num_blocks == 0) {
        dl.num_blocks = b->num_blocks;
        if (!dl.num_blocks) return dl_fail("empty UF2");
    }
    if (b->block_no != dl.blocks_done || b->num_blocks != dl.num_blocks) return dl_fail("UF2 block out of sequence");
    dl.blocks_done++;

    // Only main-flash blocks of our family are written (picotool may add
    // e.g. an "absolute" family block for the RP2350-E10 workaround).
    if ((b->flags & UF2_FLAG_NOT_MAIN_FLASH) || !(b->flags & UF2_FLAG_FAMILY_ID_PRESENT) ||
        b->file_size != RP2350_ARM_S_FAMILY_ID)
        return true;
    if (b->payload_size != FLASH_PAGE || (b->target_addr & (FLASH_PAGE - 1))) return dl_fail("bad UF2 payload");

    if (b->target_addr < XIP_BASE || b->target_addr - XIP_BASE + FLASH_PAGE > dl.part_size)
        return dl_fail("image too large for partition");

    uint32_t rel = b->target_addr - XIP_BASE;
    uint32_t sector = rel / FLASH_SECTOR;
    if (!(dl.erased[sector / 8] & (1u << (sector % 8)))) {
        if (!flash_erase_sector(dl.part_offset + sector * FLASH_SECTOR)) return dl_fail("flash erase failed");
        dl.erased[sector / 8] |= (uint8_t)(1u << (sector % 8));
    }
    if (!flash_program(dl.part_offset + rel, b->data, FLASH_PAGE)) return dl_fail("flash program failed");
    dl.wrote_any = true;
    return true;
}

static bool on_body(void *ctx, const uint8_t *data, size_t len) {
    if (!dl.sha_started) {
        if (pico_sha256_start_blocking(&dl.sha, SHA256_BIG_ENDIAN, false) != PICO_OK) return dl_fail("SHA busy");
        dl.sha_started = true;
    }
    pico_sha256_update_blocking(&dl.sha, data, len);
    dl.total += len;
    if (dl.expected_size > 0 && dl.total > (size_t)dl.expected_size) return dl_fail("file larger than manifest");

    while (len) {
        size_t take = sizeof(dl.block) - dl.fill;
        if (take > len) take = len;
        memcpy(dl.block + dl.fill, data, take);
        dl.fill += take;
        data += take;
        len -= take;
        if (dl.fill == sizeof(dl.block)) {
            dl.fill = 0;
            if (!handle_block((const struct uf2_block *)dl.block)) return false;
        }
    }
    if (dl.expected_size > 0) {
        static int last_pct = -1;
        int pct = (int)(dl.total * 100 / (size_t)dl.expected_size);
        if (pct / 10 != last_pct / 10) {
            last_pct = pct;
            char p[8];
            snprintf(p, sizeof(p), "%d%%", pct);
            set_state_text("downloading %s", p);
            printf("OTA: %s\n", p);
        }
    }
    return true;
}

static void invalidate_target(void) {
    if (dl.wrote_any && dl.have_target) {
        // A partial image must never look bootable.
        flash_erase_sector(dl.part_offset);
    }
}

static bool download_and_install(const char *version, const char *url, long size, const char *sha_hex) {
    memset(&dl, 0, sizeof(dl));
    dl.expected_size = size;
    // Partition-table queries need the bootrom SHA-256 lock, which the
    // running hash holds during the download: resolve the target first.
    if (!target_partition()) {
        printf("✗ OTA: no target partition\n");
        set_state_text("failed: %s", "no target partition");
        return false;
    }
    dl.have_target = true;
    printf("\n=== OTA: downloading %s ===\n%s\n", version, url);
    set_state_text("downloading %s", version);
    led_set(LED1, COLOR_PURPLE);

    http_result_t res;
    bool ok = http_request("GET", url, NULL, NULL, 0, on_body, NULL, &res, DOWNLOAD_TIMEOUT_MS);
    sha256_result_t digest;
    if (dl.sha_started) pico_sha256_finish(&dl.sha, &digest);

    if (ok && res.status != 200) {
        snprintf(dl.error, sizeof(dl.error), "HTTP %d", res.status);
        ok = false;
    }
    if (!ok && !dl.error[0]) str_copy(dl.error, sizeof(dl.error), "connection or TLS failed");
    if (ok && size > 0 && dl.total != (size_t)size) ok = dl_fail("size mismatch");
    if (ok && (dl.fill || !dl.num_blocks || dl.blocks_done != dl.num_blocks || !dl.wrote_any))
        ok = dl_fail("incomplete UF2");
    if (ok) {
        char hex[65];
        hex_encode(digest.bytes, sizeof(digest.bytes), hex);
        if (strcasecmp(hex, sha_hex) != 0) ok = dl_fail("SHA-256 mismatch");
    }
    status_restore_leds();

    if (!ok) {
        printf("✗ OTA: %s\n", dl.error[0] ? dl.error : "download failed");
        set_state_text("failed: %s", dl.error[0] ? dl.error : "download");
        invalidate_target();
        return false;
    }

    printf("✓ OTA: %s verified (%u bytes), rebooting into it\n", version, (unsigned)dl.total);
    str_copy(state.pending, sizeof(state.pending), version);
    state_save();
    ntrip_stop();
    led_both(COLOR_PURPLE);
    sleep_ms(1000);
    rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE, 1000, XIP_BASE + dl.part_offset, 0);
    for (;;) sleep_ms(100);
}

// A scheduled check that finds an update waits a random delay before
// downloading, so a fleet doesn't hit the server at the same moment.
static struct {
    bool active;
    absolute_time_t at;
    char version[16], url[CFG_URL], sha[72];
    long size;
} pending;

static void check_manifest(bool manual) {
    // The "rp2350" section of the shared firmware manifest (fw_manifest.h).
    static json_doc_t doc;
    switch (fw_manifest_section("rp2350", &doc)) {
    case FWM_OK:
        break;
    case FWM_UNAVAILABLE:
        set_state_text("manifest unavailable", NULL);
        return;
    case FWM_ABSENT:  // no RP2350 release published
        printf("OTA: no rp2350 section in the manifest\n");
        set_state_text("up to date", NULL);
        return;
    case FWM_DISABLED:
        return;
    default:
        printf("OTA: invalid manifest\n");
        set_state_text("invalid manifest", NULL);
        return;
    }
    char version[16] = "", file_url[CFG_URL] = "", sha[72] = "";
    double size = -1;
    int t;
    if ((t = json_find(&doc, "version")) >= 0) json_tok_str(&doc, t, version, sizeof(version));
    if ((t = json_find(&doc, "url")) >= 0) json_tok_str(&doc, t, file_url, sizeof(file_url));
    if ((t = json_find(&doc, "sha256")) >= 0) json_tok_str(&doc, t, sha, sizeof(sha));
    if ((t = json_find(&doc, "size")) >= 0) json_tok_double(&doc, t, &size);
    if (!version[0] || !file_url[0] || strlen(sha) != 64) {
        printf("OTA: manifest needs version, url and sha256\n");
        set_state_text("invalid manifest", NULL);
        return;
    }

    if (version_compare(version, FW_VERSION_STRING) <= 0) {
        printf("OTA: up to date (%s, latest %s)\n", FW_VERSION_STRING, version);
        set_state_text("up to date", NULL);
        return;
    }
    if (strcmp(version, state.failed) == 0 && state.fail_count >= OTA_MAX_FAILURES) {
        printf("OTA: %s failed %lu times, skipping\n", version, (unsigned long)state.fail_count);
        set_state_text("skipping failed %s", version);
        return;
    }
    if (!manual && pending.active && strcmp(pending.version, version) == 0) return;  // already scheduled
    uint32_t delay = manual ? 0 : random_delay_ms((uint32_t)(config.download_jitter_s > 0 ? config.download_jitter_s : 0));
    if (!delay) {
        pending.active = false;
        download_and_install(version, file_url, (long)size, sha);
        return;
    }
    pending.active = true;
    pending.at = make_timeout_time_ms(delay);
    str_copy(pending.version, sizeof(pending.version), version);
    str_copy(pending.url, sizeof(pending.url), file_url);
    str_copy(pending.sha, sizeof(pending.sha), sha);
    pending.size = (long)size;
    char msg[30];
    snprintf(msg, sizeof(msg), "%.15s in %lus", version, (unsigned long)(delay / 1000));
    printf("OTA: %s available, download in %lu s (jitter)\n", version, (unsigned long)(delay / 1000));
    set_state_text("update %s", msg);
}

bool ota_request_check(void) {
    static absolute_time_t last_request;
    if (!config.firmware_url[0]) return false;
    // Unauthenticated button: don't let it hammer the server or block core 0.
    if (!is_nil_time(last_request) && absolute_time_diff_us(last_request, get_absolute_time()) < 30 * 1000000LL)
        return true;
    last_request = get_absolute_time();
    check_requested = true;
    set_state_text("check requested", NULL);
    return true;
}

void ota_poll(void) {
    static absolute_time_t next_revert_try;
    if (fresh_update && !healthy && to_ms_since_boot(get_absolute_time()) > OTA_HEALTH_TIMEOUT_MS &&
        time_reached(next_revert_try)) {
        revert_to_previous("not healthy after 10 min");
        next_revert_try = make_timeout_time_ms(OTA_HEALTH_TIMEOUT_MS);  // only reached if it failed
    }
    if (!config.firmware_url[0] || !net_is_up()) return;
    if (fresh_update && !healthy) return;  // prove ourselves before updating again
    if (pending.active && time_reached(pending.at) && !check_requested) {
        pending.active = false;
        download_and_install(pending.version, pending.url, pending.size, pending.sha);
    }
    bool due = config.ota_interval_h > 0 && absolute_time_diff_us(get_absolute_time(), next_check) <= 0;
    if (!due && !check_requested) return;
    // The button and the first check after boot are immediate; later
    // scheduled checks get the random download delay.
    static bool boot_check = true;
    bool manual = check_requested || boot_check;
    boot_check = false;
    check_requested = false;
    if (manual) pending.active = false;

    check_manifest(manual);
    // Only reached if no update was installed (success reboots).
    uint32_t hours = config.ota_interval_h > 24 * 7 ? 24 * 7 : (uint32_t)config.ota_interval_h;
    uint32_t interval_ms = hours * 3600u * 1000u;
    if (strstr(app_status.ota_state, "failed") || strstr(app_status.ota_state, "unavailable"))
        interval_ms = RETRY_CHECK_DELAY_MS < interval_ms ? RETRY_CHECK_DELAY_MS : interval_ms;
    next_check = make_timeout_time_ms(interval_ms);
}
