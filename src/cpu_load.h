// CPU load per core, measured as the share of time not spent in sleep_ms() /
// sleep_us() (wrapped at link time, see CMakeLists.txt). Every wait in this
// firmware goes through them, so "not sleeping" is "busy". Interrupts that fire
// while a core sleeps are counted as idle; they are short (UART, DMA, PPS, W5500).
#pragma once

#include <stdint.h>

// Close the measurement window when due (call often from core 0).
void cpu_load_tick(void);
// Busy percentage of the core over the last complete 10 s window, and the
// highest window since boot, in tenths of a percent (0-1000).
uint16_t cpu_load_permille(unsigned core);
uint16_t cpu_load_peak_permille(unsigned core);
