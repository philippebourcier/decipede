// Raw flash access for the "Config" data partition and for OTA writes.
//
// All operations go through the SDK's rom_flash_op wrapper: it takes the
// bootrom flash lock, checks permissions against the partition table, and
// runs the operation inside flash_safe_execute() (core 1 parked, IRQs off).
// Never call these from inside another flash_safe_execute().
//
// Offsets are physical flash offsets (not XIP addresses): with A/B
// partitions the XIP window is translated to the running partition, so
// other partitions can't be read through XIP.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FLASH_SECTOR 4096u
#define FLASH_PAGE   256u

// Config partition (see partitions.json).
#define CONFIG_PART_OFFSET (4096u * 1024u)
#define CONFIG_PART_SIZE   (64u * 1024u)

bool flash_erase_sector(uint32_t offset);
// offset and len must be multiples of FLASH_PAGE.
bool flash_program(uint32_t offset, const void *data, size_t len);
bool flash_read(uint32_t offset, void *data, size_t len);

// Persisted blobs in the config partition. Each area is two sectors used
// alternately, with a sequence number and CRC, so a power cut during a save
// keeps the previous copy. Max payload: FLASH_SECTOR - 16 bytes.
#define BLOB_MAX (FLASH_SECTOR - 16u)

typedef enum {
    BLOB_CONFIG = 0,  // sectors 0-1
    BLOB_OTA = 1,     // sectors 2-3
    BLOB_GNSS = 2,    // sectors 4-5: UM980 firmware staging record
    BLOB_WDOG = 3,    // sectors 6-7: GNSS watchdog reset budget
} blob_area_t;

// UM980 firmware staging area, in unpartitioned flash (the partition table
// grants "absolute" rw there).
#define GNSS_STAGE_OFFSET (8u * 1024u * 1024u)
#define GNSS_STAGE_SIZE   (4u * 1024u * 1024u)

// Returns payload length, or -1 if no valid blob.
int blob_load(blob_area_t area, void *buf, size_t buf_size);
bool blob_save(blob_area_t area, const void *data, size_t len);

uint32_t crc32_update(uint32_t crc, const void *data, size_t len);
