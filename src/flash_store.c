#include "flash_store.h"

#include <stdio.h>
#include <string.h>

#include "hardware/regs/addressmap.h"
#include "pico/bootrom.h"

#define BLOB_MAGIC 0x434b5452u  // "RTKC"

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t len;
    uint32_t crc;
} blob_hdr_t;

static bool run_op(uint32_t op, uint32_t offset, void *buf, uint32_t len) {
    cflash_flags_t flags = {
        .flags = (op << CFLASH_OP_LSB) |
                 (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB) |
                 (CFLASH_ASPACE_VALUE_STORAGE << CFLASH_ASPACE_LSB),
    };
    int rc = rom_flash_op(flags, XIP_BASE + offset, len, buf);
    if (rc != 0) {
        printf("flash: op %lu at 0x%06lx len %lu failed (%d)\n",
               (unsigned long)op, (unsigned long)offset, (unsigned long)len, rc);
        return false;
    }
    return true;
}

bool flash_erase_sector(uint32_t offset) {
    return run_op(CFLASH_OP_VALUE_ERASE, offset, NULL, FLASH_SECTOR);
}

bool flash_program(uint32_t offset, const void *data, size_t len) {
    return run_op(CFLASH_OP_VALUE_PROGRAM, offset, (void *)data, len);
}

bool flash_read(uint32_t offset, void *data, size_t len) {
    return run_op(CFLASH_OP_VALUE_READ, offset, data, len);
}

uint32_t crc32_update(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

static uint8_t sector_buf[FLASH_SECTOR] __attribute__((aligned(4)));

static uint32_t slot_offset(blob_area_t area, int idx) {
    return CONFIG_PART_OFFSET + ((uint32_t)area * 2 + (uint32_t)idx) * FLASH_SECTOR;
}

// Read slot idx of an area; true if the whole blob is valid.
static bool read_slot(blob_area_t area, int idx, blob_hdr_t *hdr) {
    if (!flash_read(slot_offset(area, idx), sector_buf, FLASH_SECTOR)) return false;
    memcpy(hdr, sector_buf, sizeof(*hdr));
    if (hdr->magic != BLOB_MAGIC || hdr->len > BLOB_MAX) return false;
    return crc32_update(0, sector_buf + sizeof(*hdr), hdr->len) == hdr->crc;
}

// Pick the newest valid slot; returns -1 if none.
static int newest_slot(blob_area_t area, blob_hdr_t *out) {
    blob_hdr_t h[2];
    bool ok[2];
    for (int i = 0; i < 2; i++) ok[i] = read_slot(area, i, &h[i]);
    int best = -1;
    if (ok[0] && ok[1]) best = (int32_t)(h[1].seq - h[0].seq) > 0 ? 1 : 0;
    else if (ok[0]) best = 0;
    else if (ok[1]) best = 1;
    if (best >= 0 && out) *out = h[best];
    return best;
}

int blob_load(blob_area_t area, void *buf, size_t buf_size) {
    blob_hdr_t hdr;
    int slot = newest_slot(area, &hdr);
    if (slot < 0) return -1;
    // sector_buf holds the last slot read; re-read the chosen one.
    if (!read_slot(area, slot, &hdr) || hdr.len > buf_size) return -1;
    memcpy(buf, sector_buf + sizeof(hdr), hdr.len);
    return (int)hdr.len;
}

bool blob_save(blob_area_t area, const void *data, size_t len) {
    if (len > BLOB_MAX) return false;
    blob_hdr_t prev;
    int slot = newest_slot(area, &prev);
    int target = slot < 0 ? 0 : slot ^ 1;
    blob_hdr_t hdr = {
        .magic = BLOB_MAGIC,
        .seq = slot < 0 ? 1 : prev.seq + 1,
        .len = (uint32_t)len,
        .crc = crc32_update(0, data, len),
    };
    memset(sector_buf, 0xFF, sizeof(sector_buf));
    memcpy(sector_buf, &hdr, sizeof(hdr));
    memcpy(sector_buf + sizeof(hdr), data, len);
    size_t prog_len = (sizeof(hdr) + len + FLASH_PAGE - 1) & ~(FLASH_PAGE - 1);
    uint32_t off = slot_offset(area, target);
    return flash_erase_sector(off) && flash_program(off, sector_buf, prog_len);
}
