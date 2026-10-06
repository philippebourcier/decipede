// UTC clock from PPS + the UM980's RECTIMEA message.
//
// utc_sync() waits for a PPS edge, queries RECTIMEA and labels that edge
// with the reported UTC second. utc_now_ms() then extrapolates from the
// latest PPS edge with the RP2350 timer, so the time is accurate to the
// interrupt latency (microseconds), not to the serial query.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Re-label the second (core 0, blocks up to ~3.5 s). Returns true if the
// receiver reported valid UTC and PPS is present.
bool utc_sync(void);
// Current UTC in ms since 1970, or false if not known.
bool utc_now_ms(int64_t *out);
// UTC (us since 1970) of a past or present time_us_64() timestamp.
bool utc_at_us(uint64_t t_us, int64_t *unix_us);
// Unix seconds of the last labelled second (for NTP's reference timestamp).
bool utc_last_label(int64_t *unix_s);
// "2026-10-06T09:41:07.123Z" or "-" when unknown.
void utc_now_iso(char *out, unsigned size);
// The query itself got an answer (receiver alive), whatever the time state.
bool utc_last_query_answered(void);
