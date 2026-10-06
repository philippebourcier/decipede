// Core 0 stack: a 16 KB stack in main RAM (the SDK's default lives in a
// 4 KB scratch bank, too small for TLS handshakes), protected by the
// ARMv8-M stack limit register (an overflow faults instead of corrupting
// memory), and painted so its high-water mark can be reported.
#pragma once

#include <stdint.h>

#define MAIN_STACK_SIZE (16 * 1024)

// Paint the big stack, switch core 0 to it and run fn (never returns).
void __attribute__((noreturn)) stack_run(int (*fn)(void));
uint32_t stack_free_min(void);  // bytes never used since boot
