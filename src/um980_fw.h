// UM980 firmware upgrade.
//
// The "um980" section of the firmware manifest at config.firmware_url (see
// fw_manifest.h) describes the receiver firmware:
//   {"model":"UM980","version":"R4.10Build20739","url":"http://…/x.pkg",
//    "size":3038408,"sha256":"…","force":false}
// A newer package (or any, with "force") is downloaded into a staging area
// in flash and verified (size, SHA-256, .pkg magic and product name). It is
// flashed into the UM980 through its serial bootloader inside
// config.maint_window (UTC), or at once from the status page button, and
// only once per package: a package that was
// installed or failed is not tried again until the manifest changes.
#pragma once

#include <stdbool.h>

void gnss_fw_init(void);
// Periodic work (core 0): manifest checks, staging, upgrade in the window.
// *um980_ok is the receiver state; it is updated after an upgrade. If an
// earlier attempt left the receiver silent, the poll retries the flash
// (rescue, up to 3 attempts in total, 10 min apart).
void gnss_fw_poll(bool *um980_ok);
// Check the manifest on the next poll (status page button).
void gnss_fw_request_check(void);
// True while an upgrade is running (watchdog and streaming suspended).
bool gnss_fw_busy(void);
const char *gnss_fw_state(void);
