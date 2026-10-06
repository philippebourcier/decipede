#include "wdt.h"

#include <stdio.h>

#include "hardware/watchdog.h"
#include "pico/time.h"

// Core 1 may block for up to ~65 s (409 back-off is split into 7 s steps,
// DNS lookups take a few seconds), so allow a generous margin.
#define CORE1_STALL_LIMIT_MS 90000

static volatile uint32_t core1_last_beat_ms;
static volatile bool core1_started;
static bool started;
static bool stall_reported;

void wdt_start(void) {
    // pause_on_debug so a debugger halt doesn't reset the chip.
    watchdog_enable(WDT_TIMEOUT_MS, true);
    started = true;
}

void wdt_core1_heartbeat(void) {
    core1_last_beat_ms = to_ms_since_boot(get_absolute_time());
    core1_started = true;
}

void wdt_feed(void) {
    if (!started) return;
    if (core1_started) {
        uint32_t age = to_ms_since_boot(get_absolute_time()) - core1_last_beat_ms;
        if (age > CORE1_STALL_LIMIT_MS) {
            if (!stall_reported) {
                printf("✗ Core 1 stalled for %lu ms, letting the watchdog reset\n", (unsigned long)age);
                stall_reported = true;
            }
            return;
        }
    }
    watchdog_update();
}

bool wdt_caused_reboot(void) {
    return watchdog_enable_caused_reboot();
}
