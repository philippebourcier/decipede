// Minimal HTTP/1.0 client over a W5500 socket (core 0 only). http:// and
// https:// (TLS 1.2 with certificate verification, see tls.h).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool tls;
    char host[64];
    uint16_t port;
    char path[192];
} url_t;

bool url_parse(const char *url, url_t *out);

// Return false to abort the transfer.
typedef bool (*http_body_cb_t)(void *ctx, const uint8_t *data, size_t len);

typedef struct {
    int status;              // HTTP status code, 0 if none received
    long content_length;     // -1 if absent
    size_t body_bytes;       // body bytes delivered to the callback
} http_result_t;

// Perform a request. Body (may be NULL) is sent with content_type. The
// response body is streamed to on_body. Returns false on network/protocol
// failure (an HTTP error status still returns true; check res->status).
bool http_request(const char *method, const char *url, const char *content_type,
                  const char *body, size_t body_len, http_body_cb_t on_body, void *ctx,
                  http_result_t *res, uint32_t timeout_ms);

// GET into a buffer. Returns body length, or -1 on failure/overflow.
int http_get_to_buffer(const char *url, char *buf, size_t buf_size, int *status, uint32_t timeout_ms);

// Fire-and-forget POST of a JSON body. Returns true if the server answered.
bool http_post_json(const char *url, const char *json, uint32_t timeout_ms);
