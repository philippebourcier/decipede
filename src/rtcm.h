// RTCM3 support: the UM980 message plan (rtcm_params.py) and a streaming
// frame checker (CRC-24Q, rtcm_decoder.py) used for statistics only. The
// byte stream is forwarded to the caster unchanged.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;     // e.g. "RTCM1005"
    uint16_t interval_s;  // 0 = use config.rtcm_interval (observations)
} rtcm_msg_t;

extern const rtcm_msg_t rtcm_messages[];
extern const size_t rtcm_message_count;

typedef struct {
    uint8_t frame[1029];  // max frame: 3 header + 1023 payload + 3 CRC
    size_t have;
    size_t need;
    uint32_t frames;
    uint32_t crc_errors;
    uint16_t last_type;
} rtcm_parser_t;

void rtcm_parser_init(rtcm_parser_t *p);
void rtcm_parser_feed(rtcm_parser_t *p, const uint8_t *data, size_t len);
uint32_t rtcm_crc24q(const uint8_t *data, size_t len);
