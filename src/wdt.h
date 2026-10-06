// Hardware watchdog (15 s, the RP2350 maximum is ~16.7 s), fed from core 0.
//
// Core 1 (NTRIP) reports liveness with wdt_core1_heartbeat(); once core 1
// has started, core 0 stops feeding if core 1 has been silent for too long,
// so a hung NTRIP core also resets the board.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Leaves room for a failing ioLibrary DNS lookup (~9 s, blocking).
#define WDT_TIMEOUT_MS 15000

void wdt_start(void);
void wdt_feed(void);
void wdt_core1_heartbeat(void);
// True if the last reset was caused by the watchdog.
bool wdt_caused_reboot(void);
