#include "utc.h"

#include <stdio.h>
#include <string.h>

#include "gnss_io.h"
#include "gnss_logic.h"
#include "idle.h"
#include "pico/time.h"
#include "um980.h"

// RECTIMEA reports the epoch whose PPS edge fired just before the log line.
// If testing ever showed a constant one-second offset, this is the knob.
#define LABEL_OFFSET_S 0
// Query this long after the edge, so the receiver has computed the epoch.
#define QUERY_DELAY_MS 300
// Labels older than this are not trusted (RP2350 crystal drift).
#define MAX_LABEL_AGE_US (3600ull * 1000000ull)
// Keep time this long after the last PPS edge.
#define HOLDOVER_US (60ull * 1000000ull)

static bool have_label;
static int64_t label_unix_s;
static uint64_t label_edge_us;
static bool answered;

bool utc_sync(void) {
    answered = false;
    // Send the query mid-second, well away from the edges.
    uint64_t edge = pps_wait_edge(1500);
    if (edge) idle_sleep_ms(QUERY_DELAY_MS);
    uint64_t tag_us;
    const char *r = um980_query_log("RECTIMEA", "#RECTIMEA", 2500, &tag_us);
    answered = r != NULL;
    rectime_t rt;
    if (!edge || !r || !rectime_parse(r, &rt)) return false;
    if (!rt.utc_valid || rt.unix_ms % 1000 != 0) return false;

    // The log is printed at the epoch it describes: it belongs to the PPS
    // edge that most recently fired before it started arriving.
    pps_info_t p;
    pps_get(&p);
    if (!p.last_edge_us || p.last_edge_us > tag_us || tag_us - p.last_edge_us > 900000) return false;

    label_unix_s = rt.unix_ms / 1000 + LABEL_OFFSET_S;
    label_edge_us = p.last_edge_us;
    have_label = true;
    return true;
}

bool utc_at_us(uint64_t t_us, int64_t *unix_us) {
    if (!have_label) return false;
    pps_info_t p;
    pps_get(&p);
    uint64_t now = time_us_64();
    if (now - label_edge_us > MAX_LABEL_AGE_US) return false;
    uint64_t edge = p.last_edge_us >= label_edge_us ? p.last_edge_us : label_edge_us;
    if (now - edge > HOLDOVER_US) return false;
    int64_t secs = (int64_t)((edge - label_edge_us + 500000) / 1000000);
    *unix_us = (label_unix_s + secs) * 1000000 + ((int64_t)t_us - (int64_t)edge);
    return true;
}

bool utc_last_label(int64_t *unix_s) {
    if (!have_label) return false;
    *unix_s = label_unix_s;
    return true;
}

bool utc_now_ms(int64_t *out) {
    if (!have_label) return false;
    pps_info_t p;
    pps_get(&p);
    uint64_t now = time_us_64();
    if (now - label_edge_us > MAX_LABEL_AGE_US) return false;
    uint64_t edge = p.last_edge_us >= label_edge_us ? p.last_edge_us : label_edge_us;
    if (now - edge > HOLDOVER_US) return false;
    // Whole seconds between the labelled edge and the latest edge.
    int64_t secs = (int64_t)((edge - label_edge_us + 500000) / 1000000);
    *out = (label_unix_s + secs) * 1000 + (int64_t)((now - edge) / 1000);
    return true;
}

void utc_now_iso(char *out, unsigned size) {
    int64_t ms;
    if (utc_now_ms(&ms)) utc_format_iso(ms, out, size);
    else snprintf(out, size, "-");
}

bool utc_last_query_answered(void) {
    return answered;
}
