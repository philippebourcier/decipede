// Cooperative idle hook for core 0.
//
// Long blocking operations on core 0 (IMU sampling, UM980 queries, HTTP
// downloads) call idle_poll() or idle_sleep_ms() so the watchdog stays fed
// and background work (button, DHCP lease, status web page) keeps running.
#pragma once

#include <stdint.h>

typedef void (*idle_fn_t)(void);

// Install the background work function (called at most every ~20 ms).
void idle_set_handler(idle_fn_t fn);
// Feed the watchdog and run background work if due.
void idle_poll(void);
// Sleep while polling.
void idle_sleep_ms(uint32_t ms);
