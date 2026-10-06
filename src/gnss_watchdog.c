#include "gnss_watchdog.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "flash_store.h"
#include "gnss_io.h"
#include "um980.h"
#include "um980_fw.h"
#include "util.h"

#define WDOG_TAG 0x57444731u  // "WDG1"

typedef struct {
    uint32_t tag;
    uint32_t resets_since_healthy;
} wdog_rec_t;

static wdog_state_t st;
static uint32_t last_rx;
static char state_text[40] = "starting";

void gnss_watchdog_init(void) {
    memset(&st, 0, sizeof(st));
    wdog_rec_t r;
    if (blob_load(BLOB_WDOG, &r, sizeof(r)) == (int)sizeof(r) && r.tag == WDOG_TAG)
        st.resets_since_healthy = r.resets_since_healthy;
    last_rx = um980_data_rx_total();
    if (st.resets_since_healthy)
        printf("GNSS watchdog: %lu reset(s) used since last 24 h of health\n", (unsigned long)st.resets_since_healthy);
}

static void persist(void) {
    wdog_rec_t r = {.tag = WDOG_TAG, .resets_since_healthy = st.resets_since_healthy};
    if (!blob_save(BLOB_WDOG, &r, sizeof(r))) printf("GNSS watchdog: failed to save state\n");
}

wdog_action_t gnss_watchdog_minute(bool com1_ok) {
    uint32_t rx = um980_data_rx_total();
    bool flowing = rx != last_rx;
    last_rx = rx;
    if (gnss_fw_busy()) return WDOG_NONE;

    wdog_inputs_t in = {
        .now_s = uptime_s() ? uptime_s() : 1,
        .com1_ok = com1_ok,
        .pps_fresh = pps_fresh(),
        .com2_flowing = flowing,
    };
    bool changed;
    wdog_action_t a = wdog_step(&st, &in, &changed);
    if (a == WDOG_RESET && (!config.gnss_watchdog || !gnss_reset_available())) {
        // Disabled or no reset line: report only, don't spend the budget.
        st.resets_since_healthy--;
        st.last_reset_s = 0;
        a = WDOG_NONE;
        changed = false;
    }
    if (a == WDOG_RECONFIGURE && !config.gnss_watchdog) a = WDOG_NONE;
    if (changed) persist();

    str_copy(state_text, sizeof(state_text), wdog_describe(&st, &in));
    if (a == WDOG_RESET)
        printf("⚠ GNSS watchdog: resetting UM980 (%s), reset %lu/%d\n", state_text,
               (unsigned long)st.resets_since_healthy, WDOG_MAX_RESETS);
    else if (a == WDOG_RECONFIGURE)
        printf("⚠ GNSS watchdog: %s for 5 min, re-checking UM980 config\n", state_text);
    return a;
}

const char *gnss_watchdog_state(void) {
    return state_text;
}

uint32_t gnss_watchdog_resets(void) {
    return st.resets_since_healthy;
}
