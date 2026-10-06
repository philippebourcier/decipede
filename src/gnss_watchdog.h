// GNSS watchdog: once a minute, decide (gnss_logic.c wdog_step) whether the
// UM980 needs a config re-check or a reset. Anti-loop rules: no reset in the
// first 10 min after boot, >= 15 min between resets, at most 3 resets until
// 24 h of continuous health (budget persisted across reboots). A missing
// PPS alone (antenna, sky) never causes a reset.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gnss_logic.h"

void gnss_watchdog_init(void);
// com1_ok: the receiver answered on its control port this minute.
// The caller performs the returned action.
wdog_action_t gnss_watchdog_minute(bool com1_ok);
const char *gnss_watchdog_state(void);
uint32_t gnss_watchdog_resets(void);
