// The firmware manifest at config.firmware_url, one file for both chips:
//
//   {"rp2350": {"version": "1.7.0", "url": ".../rtkbase-1.7.0.uf2", "size": N, "sha256": "..."},
//    "um980":  {"model": "UM980", "version": "R4.10Build20739", "url": ".../x.pkg",
//               "size": N, "sha256": "..." [, "force": true, "id": "..."]}}
//
// Either section may be absent (nothing published for that chip). Written by
// tools/make_manifest.py. Fetched with ?b=<hardware id>&fw=<RP2350 version>
// &um980=<UM980 version>, so a server can answer per device.
#pragma once

#include <stdbool.h>

#include "json_util.h"

typedef enum {
    FWM_OK,           // *section holds the chip's object
    FWM_DISABLED,     // no firmware_url configured
    FWM_UNAVAILABLE,  // download failed / HTTP error
    FWM_INVALID,      // not a JSON object, or the section isn't an object
    FWM_ABSENT,       // no section for this chip
} fwm_status_t;

// Section "rp2350" or "um980". The download is cached for 60 s, so back-to-back
// checks (the button: UM980, then RP2350) fetch the file once. Core 0 only.
fwm_status_t fw_manifest_section(const char *name, json_doc_t *section);
// Forget the cached copy (the next call downloads again).
void fw_manifest_invalidate(void);
