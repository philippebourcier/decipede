// Hardware-independent GNSS logic (host-tested in tests/host):
// RECTIME parsing and UTC arithmetic, maintenance windows, UM980 firmware
// package checks, and the GNSS watchdog decision function.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// --- UTC -----------------------------------------------------------------------

typedef struct {
    bool clock_valid;   // "clock status" field
    bool utc_valid;     // trailing "utc status" field
    int64_t unix_ms;    // UTC of the epoch, ms since 1970-01-01
} rectime_t;

// Parse a "#RECTIMEA,...;VALID,off,std,utcoff,Y,M,D,h,m,ms,VALID*crc" line
// (anywhere in buf). Returns false if no well-formed RECTIMEA is found.
bool rectime_parse(const char *buf, rectime_t *out);

// Days since 1970-01-01 for a proleptic Gregorian date.
int64_t days_from_civil(int y, unsigned m, unsigned d);
// Inverse: unix seconds -> broken-down UTC.
void civil_from_unix(int64_t unix_s, int *y, int *mo, int *d, int *h, int *mi, int *s);
// "2026-10-06T09:41:07.123Z" (needs 25 bytes).
void utc_format_iso(int64_t unix_ms, char *out, size_t size);

// --- Maintenance window --------------------------------------------------------

// spec "HH:MM-HH:MM" in UTC; the end may wrap past midnight. Returns false
// for an empty or malformed spec.
bool maint_window_contains(const char *spec, int minute_of_day);
bool maint_window_valid(const char *spec);

// --- UM980 firmware package ------------------------------------------------------

#define UM980_PKG_HEADER_SIZE 0xC0
// Check the .pkg header: magic a5 a4 a3 a2 and product name at 0x9C.
bool um980_pkg_header_ok(const uint8_t *hdr, size_t len, const char *product);
// Build number from a version string such as "R4.10Build20739"
// (returns -1 if absent).
long um980_build_number(const char *version);

// --- GNSS watchdog ---------------------------------------------------------------

#define WDOG_COM1_FAILS_FOR_RESET 3       // consecutive minute checks
#define WDOG_MUTE_BEFORE_RECONFIG_S 300   // COM2 silent with PPS present
#define WDOG_MUTE_AFTER_RECONFIG_S 300
#define WDOG_BOOT_GRACE_S 600             // no reset in the first 10 min
#define WDOG_MIN_SPACING_S 900            // 15 min between resets
#define WDOG_MAX_RESETS 3                 // until 24 h of continuous health
#define WDOG_HEALTHY_REFILL_S 86400

typedef struct {
    // Persisted across RP2350 reboots.
    uint32_t resets_since_healthy;
    // Per boot (uptime seconds, 0 = unset).
    uint32_t healthy_since_s;
    uint32_t last_reset_s;
    uint32_t com1_failures;
    uint32_t mute_since_s;
    uint32_t reconfig_at_s;
    bool gave_up;
} wdog_state_t;

typedef struct {
    uint32_t now_s;      // RP2350 uptime, seconds (> 0)
    bool com1_ok;        // UM980 answered on the control port this minute
    bool pps_fresh;      // PPS edges are arriving
    bool com2_flowing;   // RTCM bytes arrived on COM2 this minute
} wdog_inputs_t;

typedef enum { WDOG_NONE, WDOG_RECONFIGURE, WDOG_RESET } wdog_action_t;

// One decision per minute. Sets *persist_changed when resets_since_healthy
// changed and must be saved.
wdog_action_t wdog_step(wdog_state_t *st, const wdog_inputs_t *in, bool *persist_changed);
const char *wdog_describe(const wdog_state_t *st, const wdog_inputs_t *in);

// --- Satellites (SATSINFOA) ------------------------------------------------------

#define SATS_MAX 64

enum { SAT_GPS, SAT_GLO, SAT_SBAS, SAT_GAL, SAT_BDS, SAT_QZSS, SAT_IRNSS, SAT_NSYS };

typedef struct {
    uint8_t sys;    // SAT_* (system of the first signal)
    uint8_t prn;    // Unicore PRN numbering
    uint8_t el;     // degrees, 0-90
    uint8_t cn0;    // best signal C/N0, dB-Hz
    uint16_t az;    // degrees, 0-359
} sat_info_t;

typedef struct {
    uint16_t total;              // satellites reported (may exceed SATS_MAX)
    uint16_t per_sys[SAT_NSYS];  // counts per system (all reported satellites)
    uint16_t n;                  // entries stored in sat[]
    sat_info_t sat[SATS_MAX];
} sats_t;

// Parse "#SATSINFOA,...;count,ver,r,r,r,frqflag,{prn,az,el,{sys,snr,frq,nfrq}xN}...*crc"
// (anywhere in buf). Returns false if no well-formed log is found.
bool satsinfo_parse(const char *buf, sats_t *out);
// "GPS", "GLO", "SBAS", "GAL", "BDS", "QZSS", "IRNSS".
const char *sat_sys_name(unsigned sys);
