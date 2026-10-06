#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/rand.h"
#include "pico/time.h"
#include "pico/unique_id.h"

void str_copy(char *dst, size_t dst_size, const char *src) {
    if (!dst_size) return;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void str_ncopy(char *dst, size_t dst_size, const char *src, size_t n) {
    if (!dst_size) return;
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

char *str_trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

const char *str_icase_find(const char *haystack, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return haystack;
    for (; *haystack; haystack++) {
        if (strncasecmp(haystack, needle, n) == 0) return haystack;
    }
    return NULL;
}

bool str_starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool parse_ipv4(const char *s, uint8_t out[4]) {
    if (!s) return false;
    for (int i = 0; i < 4; i++) {
        if (!isdigit((unsigned char)*s)) return false;
        char *end;
        long v = strtol(s, &end, 10);
        if (v < 0 || v > 255 || end - s > 3) return false;
        out[i] = (uint8_t)v;
        s = end;
        if (i < 3) {
            if (*s != '.') return false;
            s++;
        }
    }
    return *s == '\0';
}

const char *hardware_id(void) {
    static char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    if (!id[0]) {
        pico_unique_board_id_t uid;
        pico_get_unique_board_id(&uid);
        hex_encode(uid.id, sizeof(uid.id), id);
    }
    return id;
}

void mac_from_unique_id(const uint8_t id[8], uint8_t mac[6]) {
    uint64_t x = 0;
    for (int i = 0; i < 8; i++) x = x << 8 | id[i];
    // splitmix64 finalizer: a bijection with full avalanche.
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)(x >> (8 * (5 - i)));
    mac[0] = (uint8_t)((mac[0] & 0xFC) | 0x02);  // locally administered, unicast
}

int base64_encode(const uint8_t *in, size_t len, char *out, size_t out_size) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = 4 * ((len + 2) / 3) + 1;
    if (need > out_size) return -1;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < len ? tbl[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? tbl[v & 63] : '=';
    }
    out[o] = '\0';
    return (int)o;
}

void hex_encode(const uint8_t *in, size_t len, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0xf];
    }
    out[2 * len] = '\0';
}

int version_compare(const char *a, const char *b) {
    for (int i = 0; i < 3; i++) {
        long va = a ? strtol(a, (char **)&a, 10) : 0;
        long vb = b ? strtol(b, (char **)&b, 10) : 0;
        if (va != vb) return va < vb ? -1 : 1;
        if (a && *a == '.') a++;
        if (b && *b == '.') b++;
    }
    return 0;
}

uint32_t uptime_s(void) {
    return (uint32_t)(time_us_64() / 1000000u);
}

uint32_t random_delay_ms(uint32_t max_s) {
    if (!max_s) return 0;
    if (max_s > 86400) max_s = 86400;
    return (uint32_t)(get_rand_64() % ((uint64_t)max_s * 1000 + 1));
}
