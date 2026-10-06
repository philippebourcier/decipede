// Over-the-air updates using the RP2350 bootrom A/B partitions.
//
//  - The device reads the "rp2350" section of the firmware manifest at
//    config.firmware_url (see fw_manifest.h):
//      {"version": "1.2.0", "url": "http://host/rtkbase-1.2.0.uf2",
//       "size": 412160, "sha256": "<hex of the .uf2 file>"}
//  - A newer version is streamed into the inactive partition while the
//    SHA-256 of the file is computed; on a match the board reboots into it.
//  - Stage 1 (bootrom "try before you buy"): the new image must call
//    ota_buy() within ~16.7 s or the bootrom falls back to the old image.
//  - Stage 2 (application): a freshly updated image that doesn't reach
//    ota_mark_healthy() within OTA_HEALTH_TIMEOUT_MS invalidates itself and
//    reboots into the previous image.
//  - The previous image notices the failed version on its next boot and
//    stops retrying it after OTA_MAX_FAILURES attempts.
#pragma once

#include <stdbool.h>

#define OTA_HEALTH_TIMEOUT_MS (10u * 60u * 1000u)
#define OTA_MAX_FAILURES 2

// Read boot info and persisted OTA state; detect a rollback.
void ota_boot_init(void);
// Accept a trial (TBYB) image. Call once early hardware checks pass.
void ota_buy(void);
// True while this is a trial image that hasn't been bought.
bool ota_buy_pending(void);
// The application works (network + config or NTRIP): stop the stage 2 timer.
void ota_mark_healthy(void);
// Ask for a manifest check on the next ota_poll() (status page button).
// Returns false if no firmware_url is configured.
bool ota_request_check(void);
// Periodic work: stage 2 timeout, manifest polling. Call from the main loop.
void ota_poll(void);
