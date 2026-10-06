// Small string and parsing helpers.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Bounded copy that always NUL-terminates.
void str_copy(char *dst, size_t dst_size, const char *src);
// Bounded copy of n bytes of src (not necessarily NUL-terminated).
void str_ncopy(char *dst, size_t dst_size, const char *src, size_t n);
// Trim leading and trailing whitespace in place; returns s.
char *str_trim(char *s);
// Case-insensitive substring search.
const char *str_icase_find(const char *haystack, const char *needle);
bool str_starts_with(const char *s, const char *prefix);

// Parse dotted-quad IPv4 ("192.168.1.10"). Returns false if invalid.
bool parse_ipv4(const char *s, uint8_t out[4]);

// Lowercase hex of the RP2350 unique ID, e.g. "0123456789abcdef".
const char *hardware_id(void);

// Ethernet MAC from the 64-bit chip ID: splitmix64 mix of all 64 bits,
// truncated to 48, first byte forced to locally administered + unicast
// (46 effective bits).
void mac_from_unique_id(const uint8_t id[8], uint8_t mac[6]);

// Base64 encode (for HTTP Basic auth). Returns output length, or -1 if
// out_size is too small. Output is NUL-terminated.
int base64_encode(const uint8_t *in, size_t len, char *out, size_t out_size);

// Hex encode bytes (lowercase); out needs 2*len+1 bytes.
void hex_encode(const uint8_t *in, size_t len, char *out);

// Compare "MAJOR.MINOR.PATCH" strings; returns <0, 0, >0. Unparseable
// components count as 0.
int version_compare(const char *a, const char *b);

// Random delay in [0, max_s] seconds, in ms (hardware RNG).
uint32_t random_delay_ms(uint32_t max_s);

// Seconds since boot.
uint32_t uptime_s(void);
