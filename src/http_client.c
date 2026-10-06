#include "http_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "idle.h"
#include "net.h"
#include "pico/time.h"
#include "socket.h"
#include "tls.h"
#include "wiz.h"
#include "util.h"

#define HDR_MAX 1024

bool url_parse(const char *url, url_t *out) {
    const char *p;
    if (str_starts_with(url, "http://")) {
        p = url + 7;
        out->tls = false;
    } else if (str_starts_with(url, "https://")) {
        p = url + 8;
        out->tls = true;
    } else {
        printf("ERROR: URL must start with http:// or https://\n");
        return false;
    }
    const char *rest = strpbrk(p, "/?");
    const char *host_end = rest ? rest : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(host_end - p));
    out->port = out->tls ? 443 : 80;
    if (colon) {
        out->port = (uint16_t)atoi(colon + 1);
        host_end = colon;
    }
    if (host_end == p || (size_t)(host_end - p) >= sizeof(out->host)) return false;
    str_ncopy(out->host, sizeof(out->host), p, (size_t)(host_end - p));
    if (!rest) str_copy(out->path, sizeof(out->path), "/");
    else if (*rest == '?') snprintf(out->path, sizeof(out->path), "/%s", rest);
    else str_copy(out->path, sizeof(out->path), rest);
    return out->port != 0;
}

static bool expired(absolute_time_t deadline) {
    return absolute_time_diff_us(get_absolute_time(), deadline) <= 0;
}

static bool tcp_open(const url_t *u, absolute_time_t deadline) {
    uint8_t ip[4];
    if (!net_resolve(u->host, ip)) return false;

    if (wz_socket(SOCK_HTTPC, Sn_MR_TCP, net_ephemeral_port(), SF_IO_NONBLOCK) != SOCK_HTTPC) return false;
    net_tcp_prepare(SOCK_HTTPC);
    printf("Connecting to: %d.%d.%d.%d:%u\n", ip[0], ip[1], ip[2], ip[3], u->port);
    int8_t rc = wz_connect(SOCK_HTTPC, ip, u->port);
    if (rc != SOCK_OK && rc != SOCK_BUSY) {
        wz_close(SOCK_HTTPC);
        return false;
    }
    while (getSn_SR(SOCK_HTTPC) != SOCK_ESTABLISHED) {
        uint8_t sr = getSn_SR(SOCK_HTTPC);
        if (sr == SOCK_CLOSED || expired(deadline)) {
            printf("ERROR: connect failed (%s)\n", sr == SOCK_CLOSED ? "refused/timeout" : "timeout");
            wz_close(SOCK_HTTPC);
            return false;
        }
        idle_poll();
        sleep_ms(1);
    }
    net_note_internet_ok(ip);
    return true;
}

static bool tcp_send_all(const uint8_t *data, size_t len, absolute_time_t deadline) {
    while (len) {
        uint16_t chunk = len > 2048 ? 2048 : (uint16_t)len;
        int32_t n = wz_send(SOCK_HTTPC, (uint8_t *)data, chunk);
        if (n == SOCK_BUSY) {
            if (expired(deadline)) return false;
            idle_poll();
            sleep_ms(1);
            continue;
        }
        if (n <= 0) return false;
        data += n;
        len -= (size_t)n;
    }
    return true;
}

// --- Connection: plain TCP or TLS on SOCK_HTTPC ---------------------------------

static bool conn_tls;

static bool conn_write(const uint8_t *data, size_t len, absolute_time_t deadline) {
    return conn_tls ? tls_write_all(data, len, deadline) : tcp_send_all(data, len, deadline);
}

// > 0 bytes, 0 nothing yet, -1 closed by the server, -2 error.
static int conn_read(uint8_t *buf, size_t len) {
    if (conn_tls) return tls_read(buf, len);
    uint16_t avail = getSn_RX_RSR(SOCK_HTTPC);
    if (!avail) return getSn_SR(SOCK_HTTPC) == SOCK_ESTABLISHED ? 0 : -1;
    int32_t got = wz_recv(SOCK_HTTPC, buf, avail > len ? (uint16_t)len : avail);
    return got > 0 ? (int)got : -1;
}

static void conn_close(void) {
    if (conn_tls) tls_close();
    conn_tls = false;
    wz_disconnect(SOCK_HTTPC);
    wz_close(SOCK_HTTPC);
}

static long header_content_length(const char *hdr) {
    const char *p = str_icase_find(hdr, "\r\ncontent-length:");
    return p ? strtol(p + 17, NULL, 10) : -1;
}

bool http_request(const char *method, const char *url, const char *content_type,
                  const char *body, size_t body_len, http_body_cb_t on_body, void *ctx,
                  http_result_t *res, uint32_t timeout_ms) {
    url_t u;
    memset(res, 0, sizeof(*res));
    res->content_length = -1;
    if (!url_parse(url, &u)) return false;

    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    if (!tcp_open(&u, deadline)) return false;
    conn_tls = u.tls;
    if (conn_tls && !tls_open(u.host, deadline)) {
        conn_tls = false;
        wz_close(SOCK_HTTPC);
        return false;
    }

    static char req[512];
    int n;
    if (body) {
        n = snprintf(req, sizeof(req),
                     "%s %s HTTP/1.0\r\nHost: %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n\r\n",
                     method, u.path, u.host, content_type ? content_type : "application/octet-stream",
                     (unsigned)body_len);
    } else {
        n = snprintf(req, sizeof(req), "%s %s HTTP/1.0\r\nHost: %s\r\n\r\n", method, u.path, u.host);
    }
    bool ok = n > 0 && (size_t)n < sizeof(req) && conn_write((uint8_t *)req, (size_t)n, deadline) &&
              (!body || conn_write((const uint8_t *)body, body_len, deadline));
    if (!ok) {
        printf("ERROR: HTTP send failed\n");
        conn_close();
        return false;
    }

    static char hdr[HDR_MAX + 1];
    size_t hdr_len = 0;
    bool in_body = false;
    bool aborted = false;
    static uint8_t buf[2048];

    bool failed = false;
    for (;;) {
        idle_poll();  // rate-limited; also needed while data keeps flowing (OTA)
        int got = conn_read(buf, sizeof(buf));
        if (got == 0) {
            if (expired(deadline)) {
                printf("ERROR: HTTP response timeout\n");
                conn_close();
                return false;
            }
            sleep_ms(1);
            continue;
        }
        if (got < 0) {
            failed = got == -2;
            break;  // -1: server closed, response complete
        }

        uint8_t *data = buf;
        size_t len = (size_t)got;
        if (!in_body) {
            size_t take = len < HDR_MAX - hdr_len ? len : HDR_MAX - hdr_len;
            memcpy(hdr + hdr_len, data, take);
            hdr_len += take;
            hdr[hdr_len] = '\0';
            char *end = strstr(hdr, "\r\n\r\n");
            if (!end) {
                if (hdr_len >= HDR_MAX) {
                    printf("ERROR: HTTP header too large\n");
                    conn_close();
                    return false;
                }
                continue;
            }
            size_t header_size = (size_t)(end - hdr) + 4;
            // Body bytes are those of this chunk beyond the header.
            size_t consumed_before = hdr_len - take;
            size_t body_offset = header_size - consumed_before;
            *end = '\0';
            const char *sp = strchr(hdr, ' ');
            res->status = sp ? atoi(sp + 1) : 0;
            res->content_length = header_content_length(hdr);
            in_body = true;
            data += body_offset;
            len -= body_offset;
        }
        if (len) {
            res->body_bytes += len;
            if (on_body && !on_body(ctx, data, len)) {
                aborted = true;
                break;
            }
        }
    }
    conn_close();
    if (!in_body) {
        printf("ERROR: Invalid HTTP response\n");
        return false;
    }
    return !aborted && !failed;
}

typedef struct {
    char *buf;
    size_t size;
    size_t len;
    bool overflow;
} buf_ctx_t;

static bool buf_cb(void *ctx, const uint8_t *data, size_t len) {
    buf_ctx_t *b = ctx;
    if (b->len + len > b->size) {
        b->overflow = true;
        return false;
    }
    memcpy(b->buf + b->len, data, len);
    b->len += len;
    return true;
}

int http_get_to_buffer(const char *url, char *buf, size_t buf_size, int *status, uint32_t timeout_ms) {
    buf_ctx_t b = {.buf = buf, .size = buf_size};
    http_result_t res;
    bool ok = http_request("GET", url, NULL, NULL, 0, buf_cb, &b, &res, timeout_ms);
    *status = res.status;
    if (b.overflow) {
        printf("ERROR: response larger than %u bytes\n", (unsigned)buf_size);
        return -1;
    }
    return ok ? (int)b.len : -1;
}

bool http_post_json(const char *url, const char *json, uint32_t timeout_ms) {
    http_result_t res;
    return http_request("POST", url, "application/json", json, strlen(json), NULL, NULL, &res, timeout_ms);
}
