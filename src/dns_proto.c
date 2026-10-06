#include "dns_proto.h"

#include <string.h>

size_t dns_build_query(uint16_t id, const char *host, uint8_t *out, size_t out_size) {
    size_t n = strlen(host);
    if (!n || n > 253 || out_size < 12 + n + 2 + 4) return 0;
    uint8_t *p = out;
    *p++ = (uint8_t)(id >> 8);
    *p++ = (uint8_t)id;
    *p++ = 0x01;  // RD: recursion desired
    *p++ = 0x00;
    *p++ = 0;
    *p++ = 1;     // QDCOUNT
    memset(p, 0, 6);  // AN/NS/AR counts
    p += 6;
    const char *label = host;
    while (*label) {
        const char *dot = strchr(label, '.');
        size_t len = dot ? (size_t)(dot - label) : strlen(label);
        if (len == 0 || len > 63) return 0;
        *p++ = (uint8_t)len;
        memcpy(p, label, len);
        p += len;
        label += len;
        if (*label == '.') label++;
    }
    *p++ = 0;            // root
    *p++ = 0, *p++ = 1;  // QTYPE A
    *p++ = 0, *p++ = 1;  // QCLASS IN
    return (size_t)(p - out);
}

// Skip a (possibly compressed) name; returns the offset after it, or 0.
static size_t skip_name(const uint8_t *msg, size_t len, size_t off) {
    for (int guard = 0; guard < 128; guard++) {
        if (off >= len) return 0;
        uint8_t l = msg[off];
        if (l == 0) return off + 1;
        if ((l & 0xC0) == 0xC0) return off + 2 <= len ? off + 2 : 0;  // pointer ends the name
        if (l & 0xC0) return 0;
        off += 1 + l;
    }
    return 0;
}

bool dns_parse_reply(uint16_t id, const uint8_t *msg, size_t len, uint8_t ip[4]) {
    if (len < 12) return false;
    if (((uint16_t)msg[0] << 8 | msg[1]) != id) return false;
    if (!(msg[2] & 0x80)) return false;   // QR: must be a response
    if ((msg[3] & 0x0F) != 0) return false;  // RCODE
    unsigned qd = (unsigned)msg[4] << 8 | msg[5];
    unsigned an = (unsigned)msg[6] << 8 | msg[7];
    size_t off = 12;
    for (unsigned i = 0; i < qd; i++) {
        off = skip_name(msg, len, off);
        if (!off || off + 4 > len) return false;
        off += 4;
    }
    for (unsigned i = 0; i < an; i++) {
        off = skip_name(msg, len, off);
        if (!off || off + 10 > len) return false;
        uint16_t type = (uint16_t)(msg[off] << 8 | msg[off + 1]);
        uint16_t cls = (uint16_t)(msg[off + 2] << 8 | msg[off + 3]);
        uint16_t rdlen = (uint16_t)(msg[off + 8] << 8 | msg[off + 9]);
        off += 10;
        if (off + rdlen > len) return false;
        if (type == 1 && cls == 1 && rdlen == 4) {
            memcpy(ip, msg + off, 4);
            return true;
        }
        off += rdlen;  // e.g. a CNAME before the A record
    }
    return false;
}
