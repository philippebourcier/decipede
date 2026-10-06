#include "cpu_load.h"

#include "hardware/sync.h"
#include "pico/platform.h"
#include "pico/time.h"

#define WINDOW_US 10000000u

void __real_sleep_ms(uint32_t ms);
void __real_sleep_us(uint64_t us);

static volatile uint64_t slept_us[2];
static uint64_t window_start_us;
static uint64_t window_slept_us[2];
static uint16_t load[2], peak[2];

void __wrap_sleep_ms(uint32_t ms) {
    uint64_t t = time_us_64();
    __real_sleep_ms(ms);
    slept_us[get_core_num()] += time_us_64() - t;  // each core only writes its own slot
}

void __wrap_sleep_us(uint64_t us) {
    uint64_t t = time_us_64();
    __real_sleep_us(us);
    slept_us[get_core_num()] += time_us_64() - t;
}

void cpu_load_tick(void) {
    uint64_t now = time_us_64();
    if (!window_start_us) {
        window_start_us = now;
        for (unsigned c = 0; c < 2; c++) window_slept_us[c] = slept_us[c];
        return;
    }
    uint64_t span = now - window_start_us;
    if (span < WINDOW_US) return;
    for (unsigned c = 0; c < 2; c++) {
        // 64-bit read of the other core's counter: retry if it changed mid-read.
        uint64_t s, s2;
        do {
            s = slept_us[c];
            s2 = slept_us[c];
        } while (s != s2);
        uint64_t idle = s - window_slept_us[c];
        window_slept_us[c] = s;
        if (idle > span) idle = span;  // a sleep that started before the window
        load[c] = (uint16_t)((span - idle) * 1000 / span);
        if (load[c] > peak[c]) peak[c] = load[c];
    }
    window_start_us = now;
}

uint16_t cpu_load_permille(unsigned core) {
    return core < 2 ? load[core] : 0;
}

uint16_t cpu_load_peak_permille(unsigned core) {
    return core < 2 ? peak[core] : 0;
}
