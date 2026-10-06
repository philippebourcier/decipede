// TLS 1.2 client over the HTTP client socket (mbedTLS), core 0 only.
//
// Server certificates are always verified against the compiled-in trust
// store (ISRG Root X1/X2/YR/YE + provision/ca.pem) and the hostname. The
// validity dates of the server certificate and intermediates are checked
// against the GNSS UTC clock when it is valid (without UTC, i.e. no PPS, only
// the date check is skipped); those of the trust anchors themselves are not.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pico/time.h"

// Handshake on the already connected SOCK_HTTPC.
bool tls_open(const char *host, absolute_time_t deadline);
// Send everything (false on error/timeout).
bool tls_write_all(const uint8_t *data, size_t len, absolute_time_t deadline);
// > 0: bytes read, 0: nothing yet, -1: connection closed (EOF), -2: error.
int tls_read(uint8_t *buf, size_t len);
// Send close_notify and release the session.
void tls_close(void);
// Number of trust anchors loaded (0 until the first connection).
int tls_trust_anchors(void);
