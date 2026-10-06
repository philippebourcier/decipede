// Unicore UM980 driver and base-station configuration, port of
// um980_config.py. COM1 (UART0) is the control port; COM2 (UART1) carries
// RTCM and is read by the NTRIP core via um980_data_*().
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int l1, l2, l5;  // -1 = unknown
} agc_values_t;

typedef enum { AGC_UNKNOWN, AGC_GOOD, AGC_BAD } agc_state_t;

void um980_uart_init(void);

// Detect (with retries), check config and reconfigure if needed.
// Fills model/firmware. Returns false if the receiver isn't detected.
bool um980_start(char *model, size_t model_size, char *firmware, size_t fw_size);

bool um980_get_agc(agc_values_t *out);
agc_state_t um980_agc_state(int value);

// Send a query (plain command, e.g. "RECTIMEA") on COM1 and return the raw
// response (static buffer, valid until the next call), or NULL.
const char *um980_query(const char *cmd, uint32_t timeout_ms);

// Send a query whose answer is a log line (e.g. "RECTIMEA" -> "#RECTIMEA"),
// wait for that line, and report when its first character arrived.
// Returns whatever arrived (NULL if nothing at all); *tag_time_us is 0 if
// the log line didn't come.
const char *um980_query_log(const char *cmd, const char *tag, uint32_t timeout_ms, uint64_t *tag_time_us);

// --- Raw COM1 access (bootloader) ---------------------------------------------
void um980_ctrl_set_baud(uint32_t baud);
void um980_ctrl_write(const void *data, size_t len);
void um980_ctrl_discard_input(void);
// Next received byte, or -1 after timeout_ms (keeps idle work running).
int um980_ctrl_read_byte(uint32_t timeout_ms);
// Wait until `needle` appears in the received text (or timeout).
bool um980_ctrl_wait_string(const char *needle, uint32_t timeout_ms);
// Wait for one of the given bytes; returns it, or -1 on timeout.
int um980_ctrl_wait_byte(const uint8_t *accept, size_t n, uint32_t timeout_ms);

// --- COM2 data stream (DMA ring, safe to read from core 1) ------------------
// Bytes available since the last read position. Returns 0 and resyncs if
// the writer lapped the reader (overrun count incremented).
size_t um980_data_available(uint32_t *overruns);
// Copy up to max bytes without consuming them.
size_t um980_data_peek(uint8_t *dst, size_t max);
void um980_data_consume(size_t n);
// Bytes received on COM2 since boot (approximate, safe from core 0).
uint32_t um980_data_rx_total(void);
// Drop everything buffered (stale corrections after a reconnect).
void um980_data_flush(void);
