// Minimal DNS client messages (A records over UDP), pure and host-tested.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Build a recursive A query for host. Returns the length, or 0 if the
// name doesn't fit or is malformed.
size_t dns_build_query(uint16_t id, const char *host, uint8_t *out, size_t out_size);

// Parse a reply: true and the first A record in ip[] if it answers query
// `id` successfully.
bool dns_parse_reply(uint16_t id, const uint8_t *msg, size_t len, uint8_t ip[4]);
