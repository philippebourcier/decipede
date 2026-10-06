#include "idle.h"

#include <stdbool.h>

#include "pico/time.h"
#include "wdt.h"

#define IDLE_PERIOD_US 20000

static idle_fn_t handler;
static bool in_handler;
static uint64_t last_run_us;

void idle_set_handler(idle_fn_t fn) {
    handler = fn;
}

void idle_poll(void) {
    wdt_feed();
    if (!handler || in_handler) return;
    uint64_t now = time_us_64();
    if (now - last_run_us < IDLE_PERIOD_US) return;
    last_run_us = now;
    in_handler = true;
    handler();
    in_handler = false;
}

void idle_sleep_ms(uint32_t ms) {
    absolute_time_t until = make_timeout_time_ms(ms);
    while (absolute_time_diff_us(get_absolute_time(), until) > 0) {
        idle_poll();
        sleep_ms(ms < 5 ? ms : 5);
    }
}
