#include "rtcm.h"

#include <string.h>

// Same list and intervals as rtcm_params.py. The MSM7 observation messages
// (interval 1 there) follow config.rtcm_interval, which defaults to 1.
const rtcm_msg_t rtcm_messages[] = {
    // Reference station information
    {"RTCM1005", 30},
    {"RTCM1006", 30},
    {"RTCM1033", 10},
    // Ephemerides
    {"RTCM1019", 10},
    {"RTCM1020", 10},
    {"RTCM1042", 10},
    {"RTCM1044", 10},
    {"RTCM1045", 10},
    {"RTCM1046", 10},
    // Observations (MSM7)
    {"RTCM1077", 0},
    {"RTCM1087", 0},
    {"RTCM1097", 0},
    {"RTCM1107", 0},
    {"RTCM1117", 0},
    {"RTCM1127", 0},
};
const size_t rtcm_message_count = sizeof(rtcm_messages) / sizeof(rtcm_messages[0]);

#define PREAMBLE 0xD3

uint32_t rtcm_crc24q(const uint8_t *data, size_t len) {
    uint32_t crc = 0;
    while (len--) {
        crc ^= (uint32_t)(*data++) << 16;
        for (int i = 0; i < 8; i++) {
            crc <<= 1;
            if (crc & 0x1000000) crc ^= 0x1864CFB;
        }
    }
    return crc & 0xFFFFFF;
}

void rtcm_parser_init(rtcm_parser_t *p) {
    memset(p, 0, sizeof(*p));
}

static void finish_frame(rtcm_parser_t *p) {
    size_t n = p->need;
    uint32_t rx = (uint32_t)p->frame[n - 3] << 16 | (uint32_t)p->frame[n - 2] << 8 | p->frame[n - 1];
    if (rtcm_crc24q(p->frame, n - 3) == rx) {
        p->frames++;
        if (n >= 8) p->last_type = (uint16_t)(p->frame[3] << 4 | p->frame[4] >> 4);
    } else {
        p->crc_errors++;
    }
}

void rtcm_parser_feed(rtcm_parser_t *p, const uint8_t *data, size_t len) {
    while (len) {
        if (p->have == 0) {
            // Hunt for the preamble.
            const uint8_t *d = memchr(data, PREAMBLE, len);
            if (!d) return;
            len -= (size_t)(d - data);
            data = d;
            p->frame[0] = *data++;
            len--;
            p->have = 1;
            p->need = 3;
            continue;
        }
        size_t take = p->need - p->have;
        if (take > len) take = len;
        memcpy(p->frame + p->have, data, take);
        p->have += take;
        data += take;
        len -= take;
        if (p->have < p->need) return;
        if (p->need == 3) {
            // Header complete: 6 reserved bits must be zero, 10-bit length.
            if (p->frame[1] & 0xFC) {
                p->have = 0;
                continue;
            }
            p->need = 3 + (size_t)(((p->frame[1] & 0x03) << 8) | p->frame[2]) + 3;
            continue;
        }
        finish_frame(p);
        p->have = 0;
    }
}
