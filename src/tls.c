#include "tls.h"

#include <stdio.h>
#include <string.h>

#include "gnss_logic.h"
#include "idle.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "net.h"
#include "socket.h"
#include "utc.h"
#include "wiz.h"

extern const char tls_ca_bundle[];
extern const size_t tls_ca_bundle_len;

static bool ready;
static int anchors;
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_x509_crt cacert;
static mbedtls_ssl_config conf;
static mbedtls_ssl_context ssl;
static bool session_open;

static void log_err(const char *what, int rc) {
    char msg[96];
    mbedtls_strerror(rc, msg, sizeof(msg));
    printf("ERROR: TLS %s: -0x%04x %s\n", what, (unsigned)-rc, msg);
}

// --- W5500 socket I/O for mbedTLS ------------------------------------------------

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int32_t n = wz_send(SOCK_HTTPC, (uint8_t *)buf, (uint16_t)(len > 2048 ? 2048 : len));
    if (n == SOCK_BUSY) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return n > 0 ? (int)n : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    int32_t n = wz_recv(SOCK_HTTPC, buf, (uint16_t)(len > 2048 ? 2048 : len));
    if (n > 0) return (int)n;
    if (n == SOCK_BUSY) {
        // Peer closed (CLOSE_WAIT) with nothing left to read: EOF.
        return getSn_SR(SOCK_HTTPC) == SOCK_CLOSE_WAIT ? 0 : MBEDTLS_ERR_SSL_WANT_READ;
    }
    return 0;  // socket closed
}

// --- Certificate dates against GNSS UTC ----------------------------------------------

static int64_t x509_unix(const mbedtls_x509_time *t) {
    return days_from_civil(t->year, (unsigned)t->mon, (unsigned)t->day) * 86400 + t->hour * 3600 + t->min * 60 + t->sec;
}

static bool is_trust_anchor(const mbedtls_x509_crt *crt) {
    for (const mbedtls_x509_crt *c = &cacert; c && c->raw.len; c = c->next)
        if (c == crt || (c->raw.len == crt->raw.len && !memcmp(c->raw.p, crt->raw.p, c->raw.len))) return true;
    return false;
}

// The dates of the server certificate and its intermediates are enforced. Those
// of our own trust anchors are not: they are trusted because they are compiled
// in, and a root expiring must not cut devices off from the very update that
// replaces it (the build refuses anchors close to expiry instead, see
// tools/gen_ca_bundle.py).
static int verify_dates(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    int64_t now_ms;
    if (!utc_now_ms(&now_ms)) return 0;  // no UTC: chain + hostname only
    if (is_trust_anchor(crt)) return 0;
    int64_t now = now_ms / 1000;
    if (now < x509_unix(&crt->valid_from)) *flags |= MBEDTLS_X509_BADCERT_FUTURE;
    if (now > x509_unix(&crt->valid_to)) *flags |= MBEDTLS_X509_BADCERT_EXPIRED;
    return 0;
}

static bool tls_init(void) {
    if (ready) return true;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509_crt_init(&cacert);
    mbedtls_ssl_config_init(&conf);
    static const char pers[] = "rtkbase";
    int rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)pers, sizeof(pers));
    if (rc) {
        log_err("RNG seed", rc);
        return false;
    }
    rc = mbedtls_x509_crt_parse(&cacert, (const unsigned char *)tls_ca_bundle, tls_ca_bundle_len);
    if (rc < 0) {
        log_err("trust store", rc);
        return false;
    }
    for (const mbedtls_x509_crt *c = &cacert; c && c->raw.len; c = c->next) anchors++;
    rc = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc) {
        log_err("config", rc);
        return false;
    }
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf, &cacert, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_conf_verify(&conf, verify_dates, NULL);
    ready = true;
    printf("TLS: %d trust anchor(s) loaded\n", anchors);
    return true;
}

int tls_trust_anchors(void) {
    return anchors;
}

bool tls_open(const char *host, absolute_time_t deadline) {
    if (!tls_init()) return false;
    mbedtls_ssl_init(&ssl);
    int rc = mbedtls_ssl_setup(&ssl, &conf);
    if (!rc) rc = mbedtls_ssl_set_hostname(&ssl, host);  // SNI + certificate name check
    if (rc) {
        log_err("setup", rc);
        mbedtls_ssl_free(&ssl);
        return false;
    }
    mbedtls_ssl_set_bio(&ssl, NULL, bio_send, bio_recv, NULL);
    session_open = true;

    while ((rc = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            uint32_t vr = mbedtls_ssl_get_verify_result(&ssl);
            if (vr && vr != (uint32_t)-1) {
                char info[160];
                mbedtls_x509_crt_verify_info(info, sizeof(info), "", vr);
                printf("ERROR: TLS certificate rejected for %s: %s", host, info);
            }
            log_err("handshake", rc);
            tls_close();
            return false;
        }
        if (time_reached(deadline)) {
            printf("ERROR: TLS handshake timeout\n");
            tls_close();
            return false;
        }
        idle_poll();
        sleep_ms(1);
    }
    printf("TLS: %s %s, certificate verified%s\n", mbedtls_ssl_get_version(&ssl), mbedtls_ssl_get_ciphersuite(&ssl),
           utc_now_ms(&(int64_t){0}) ? "" : " (dates not checked: no UTC)");
    return true;
}

bool tls_write_all(const uint8_t *data, size_t len, absolute_time_t deadline) {
    while (len) {
        int n = mbedtls_ssl_write(&ssl, data, len);
        if (n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ) {
            if (time_reached(deadline)) return false;
            idle_poll();
            sleep_ms(1);
            continue;
        }
        if (n <= 0) {
            log_err("write", n);
            return false;
        }
        data += n;
        len -= (size_t)n;
    }
    return true;
}

int tls_read(uint8_t *buf, size_t len) {
    int n = mbedtls_ssl_read(&ssl, buf, len);
    if (n > 0) return n;
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == MBEDTLS_ERR_SSL_CONN_EOF) return -1;
    log_err("read", n);
    return -2;
}

void tls_close(void) {
    if (!session_open) return;
    mbedtls_ssl_close_notify(&ssl);  // best effort, non-blocking socket
    mbedtls_ssl_free(&ssl);
    session_open = false;
}
