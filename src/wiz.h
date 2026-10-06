// Cross-core safe wrappers for the ioLibrary socket API.
//
// ioLibrary keeps per-socket state in shared bitmasks (sock_io_mode,
// sock_is_sending, sock_any_port) updated with plain read-modify-write, so
// calls from core 0 and core 1 must not interleave. Every socket API call
// goes through these wrappers, which hold the (recursive) W5500 lock.
// Register accessors (getSn_SR etc.) are already locked per access.
#pragma once

#include "net.h"
#include "socket.h"

static inline int8_t wz_socket(uint8_t sn, uint8_t protocol, uint16_t port, uint8_t flag) {
    net_lock();
    int8_t r = socket(sn, protocol, port, flag);
    net_unlock();
    return r;
}

static inline int8_t wz_connect(uint8_t sn, uint8_t *ip, uint16_t port) {
    net_lock();
    int8_t r = connect(sn, ip, port);
    net_unlock();
    return r;
}

static inline int8_t wz_listen(uint8_t sn) {
    net_lock();
    int8_t r = listen(sn);
    net_unlock();
    return r;
}

static inline int32_t wz_send(uint8_t sn, uint8_t *buf, uint16_t len) {
    net_lock();
    int32_t r = send(sn, buf, len);
    net_unlock();
    return r;
}

// TCP receive. Not ioLibrary's recv(): at commit 3e01f80 (non-IPv6 builds)
// it returns SOCK_BUSY for non-blocking sockets before looking at the RX
// buffer, so data is only delivered once the peer has closed the
// connection. This is the W5500 path of recv() without that bug.
static inline int32_t wz_recv(uint8_t sn, uint8_t *buf, uint16_t len) {
    net_lock();
    uint16_t avail = getSn_RX_RSR(sn);
    if (avail == 0) {
        uint8_t sr = getSn_SR(sn);
        net_unlock();
        return (sr == SOCK_ESTABLISHED || sr == SOCK_CLOSE_WAIT) ? SOCK_BUSY : SOCKERR_SOCKSTATUS;
    }
    if (len > avail) len = avail;
    wiz_recv_data(sn, buf, len);
    setSn_CR(sn, Sn_CR_RECV);
    while (getSn_CR(sn)) {
    }
    net_unlock();
    return (int32_t)len;
}

static inline int8_t wz_disconnect(uint8_t sn) {
    net_lock();
    int8_t r = disconnect(sn);
    net_unlock();
    return r;
}

static inline int8_t wz_close(uint8_t sn) {
    net_lock();
    int8_t r = close(sn);
    net_unlock();
    return r;
}

static inline int32_t wz_recvfrom(uint8_t sn, uint8_t *buf, uint16_t len, uint8_t *addr, uint16_t *port) {
    net_lock();
    int32_t r = recvfrom(sn, buf, len, addr, port);
    net_unlock();
    return r;
}

static inline int32_t wz_sendto(uint8_t sn, uint8_t *buf, uint16_t len, uint8_t *addr, uint16_t port) {
    net_lock();
    int32_t r = sendto(sn, buf, len, addr, port);
    net_unlock();
    return r;
}
