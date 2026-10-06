#include "gnss_logic.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- UTC -----------------------------------------------------------------------

int64_t days_from_civil(int y, unsigned m, unsigned d) {
    // Howard Hinnant's algorithm.
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void civil_from_unix(int64_t unix_s, int *y, int *mo, int *d, int *h, int *mi, int *s) {
    int64_t days = unix_s >= 0 ? unix_s / 86400 : (unix_s - 86399) / 86400;
    int64_t secs = unix_s - days * 86400;
    *h = (int)(secs / 3600);
    *mi = (int)(secs / 60 % 60);
    *s = (int)(secs % 60);
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = (unsigned)(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *mo = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*mo <= 2));
}

void utc_format_iso(int64_t unix_ms, char *out, size_t size) {
    int y, mo, d, h, mi, s;
    int64_t sec = unix_ms >= 0 ? unix_ms / 1000 : (unix_ms - 999) / 1000;
    civil_from_unix(sec, &y, &mo, &d, &h, &mi, &s);
    snprintf(out, size, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", y, mo, d, h, mi, s, (int)(unix_ms - sec * 1000));
}

bool rectime_parse(const char *buf, rectime_t *out) {
    const char *p = strstr(buf, "#RECTIMEA");
    if (!p) return false;
    const char *semi = strchr(p, ';');
    if (!semi) return false;
    char status[12], utc_status[12];
    double off, std, utcoff;
    int y, mo, d, h, mi;
    long ms;
    // Field 1 is the clock status word; the last one (before '*') the UTC status.
    int n = sscanf(semi + 1, "%11[^,],%lf,%lf,%lf,%d,%d,%d,%d,%d,%ld,%11[^*]", status, &off, &std, &utcoff, &y, &mo,
                   &d, &h, &mi, &ms, utc_status);
    if (n != 11) return false;
    out->clock_valid = strcmp(status, "VALID") == 0;
    out->utc_valid = strcmp(utc_status, "VALID") == 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || ms < 0 || ms > 60999 ||
        y < 2020 || y > 2100) {
        out->utc_valid = false;
        out->unix_ms = 0;
        return true;
    }
    out->unix_ms = (days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60) * 1000 + ms;
    return true;
}

// --- Maintenance window --------------------------------------------------------

static bool parse_hhmm(const char *s, int *minutes, const char **end) {
    if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1]) || s[2] != ':' ||
        !isdigit((unsigned char)s[3]) || !isdigit((unsigned char)s[4]))
        return false;
    int h = (s[0] - '0') * 10 + (s[1] - '0');
    int m = (s[3] - '0') * 10 + (s[4] - '0');
    if (h > 23 || m > 59) return false;
    *minutes = h * 60 + m;
    *end = s + 5;
    return true;
}

static bool parse_window(const char *spec, int *start, int *end) {
    const char *p;
    if (!spec || !parse_hhmm(spec, start, &p) || *p != '-') return false;
    if (!parse_hhmm(p + 1, end, &p) || *p) return false;
    return *start != *end;
}

bool maint_window_valid(const char *spec) {
    int a, b;
    return parse_window(spec, &a, &b);
}

bool maint_window_contains(const char *spec, int minute_of_day) {
    int start, end;
    if (!parse_window(spec, &start, &end)) return false;
    if (start < end) return minute_of_day >= start && minute_of_day < end;
    return minute_of_day >= start || minute_of_day < end;  // wraps midnight
}

// --- UM980 firmware package ------------------------------------------------------

bool um980_pkg_header_ok(const uint8_t *hdr, size_t len, const char *product) {
    static const uint8_t magic[4] = {0xa5, 0xa4, 0xa3, 0xa2};
    size_t plen = strlen(product);
    if (len < UM980_PKG_HEADER_SIZE || memcmp(hdr, magic, 4) != 0) return false;
    return memcmp(hdr + 0x9C, product, plen) == 0 && hdr[0x9C + plen] == 0;
}

long um980_build_number(const char *version) {
    const char *p = version ? strstr(version, "Build") : NULL;
    if (!p || !isdigit((unsigned char)p[5])) return -1;
    return strtol(p + 5, NULL, 10);
}

// --- GNSS watchdog ---------------------------------------------------------------

wdog_action_t wdog_step(wdog_state_t *st, const wdog_inputs_t *in, bool *persist_changed) {
    uint32_t now = in->now_s;
    *persist_changed = false;

    st->com1_failures = in->com1_ok ? 0 : st->com1_failures + 1;

    // "Alive but mute": the receiver answers and has a time lock (PPS) but
    // no RTCM comes out of COM2. A missing PPS (antenna, sky) never counts.
    bool mute = in->com1_ok && in->pps_fresh && !in->com2_flowing;
    if (mute) {
        if (!st->mute_since_s) st->mute_since_s = now;
    } else {
        st->mute_since_s = 0;
        st->reconfig_at_s = 0;
    }

    // Budget refill after 24 h of continuous, complete health.
    bool healthy = in->com1_ok && in->pps_fresh && in->com2_flowing;
    if (healthy) {
        if (!st->healthy_since_s) st->healthy_since_s = now;
        if (now - st->healthy_since_s >= WDOG_HEALTHY_REFILL_S && (st->resets_since_healthy || st->gave_up)) {
            st->resets_since_healthy = 0;
            st->gave_up = false;
            *persist_changed = true;
        }
    } else {
        st->healthy_since_s = 0;
    }

    if (st->mute_since_s && !st->reconfig_at_s && now - st->mute_since_s >= WDOG_MUTE_BEFORE_RECONFIG_S) {
        st->reconfig_at_s = now;
        return WDOG_RECONFIGURE;  // cheap fix first: re-check the UM980 config
    }

    bool want_reset = st->com1_failures >= WDOG_COM1_FAILS_FOR_RESET ||
                      (st->reconfig_at_s && now - st->reconfig_at_s >= WDOG_MUTE_AFTER_RECONFIG_S);
    if (!want_reset) return WDOG_NONE;
    if (now < WDOG_BOOT_GRACE_S) return WDOG_NONE;
    if (st->last_reset_s && now - st->last_reset_s < WDOG_MIN_SPACING_S) return WDOG_NONE;
    if (st->resets_since_healthy >= WDOG_MAX_RESETS) {
        st->gave_up = true;
        return WDOG_NONE;
    }

    st->resets_since_healthy++;
    *persist_changed = true;
    st->last_reset_s = now;
    st->com1_failures = 0;
    st->mute_since_s = 0;
    st->reconfig_at_s = 0;
    return WDOG_RESET;
}

const char *wdog_describe(const wdog_state_t *st, const wdog_inputs_t *in) {
    if (st->gave_up) return "gave up (reset budget used)";
    if (!in->com1_ok) return "UM980 not answering";
    if (!in->pps_fresh) return "no PPS (antenna/sky?)";
    if (!in->com2_flowing) return "no RTCM on COM2";
    return "OK";
}

// --- Satellites (SATSINFOA) ------------------------------------------------------

static bool next_long(const char **p, long *v) {
    char *end;
    *v = strtol(*p, &end, 10);
    if (end == *p || (*end != ',' && *end != '*')) return false;
    *p = *end == ',' ? end + 1 : end;
    return true;
}

bool satsinfo_parse(const char *buf, sats_t *out) {
    memset(out, 0, sizeof(*out));
    const char *p = strstr(buf, "#SATSINFOA");
    if (!p) return false;
    p = strchr(p, ';');
    if (!p) return false;
    p++;
    long count, skip;
    if (!next_long(&p, &count) || count < 0 || count > 255) return false;
    for (int i = 0; i < 5; i++)  // version, 3 reserved, frequency flag
        if (!next_long(&p, &skip)) return false;
    for (long s = 0; s < count; s++) {
        long prn, az, el, sys, snr, frq, nfrq;
        if (!next_long(&p, &prn) || !next_long(&p, &az) || !next_long(&p, &el)) return false;
        // The first signal block carries the number of signal blocks.
        if (!next_long(&p, &sys) || !next_long(&p, &snr) || !next_long(&p, &frq) || !next_long(&p, &nfrq) ||
            nfrq < 1 || nfrq > 16)
            return false;
        long best = snr;
        for (long f = 1; f < nfrq; f++) {
            long s2, snr2, frq2, n2;
            if (!next_long(&p, &s2) || !next_long(&p, &snr2) || !next_long(&p, &frq2) || !next_long(&p, &n2))
                return false;
            if (snr2 > best) best = snr2;
        }
        if (sys < 0 || sys >= SAT_NSYS) continue;
        out->total++;
        out->per_sys[sys]++;
        if (out->n < SATS_MAX) {
            sat_info_t *si = &out->sat[out->n++];
            si->sys = (uint8_t)sys;
            si->prn = (uint8_t)(prn < 0 || prn > 255 ? 0 : prn);
            si->az = (uint16_t)(az < 0 || az > 359 ? 0 : az);
            si->el = (uint8_t)(el < 0 ? 0 : el > 90 ? 90 : el);
            si->cn0 = (uint8_t)(best < 0 ? 0 : best > 99 ? 99 : best);
        }
    }
    return *p == '*';
}

const char *sat_sys_name(unsigned sys) {
    static const char *const names[SAT_NSYS] = {"GPS", "GLO", "SBAS", "GAL", "BDS", "QZSS", "IRNSS"};
    return sys < SAT_NSYS ? names[sys] : "?";
}
