#include "ntrip.h"

#include <stdio.h>
#include <string.h>

#include "app_status.h"
#include "net.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "rtcm.h"
#include "socket.h"
#include "wiz.h"
#include "um980.h"
#include "util.h"
#include "idle.h"
#include "wdt.h"

#define RETRY_DELAY_MS      7000
#define RETRY_DELAY_409_MS  60000
#define CONNECT_TIMEOUT_MS  10000
#define RESPONSE_TIMEOUT_MS 5000
// Reconnect when queued data gets no TCP ACK for this long (a dead path).
// The W5500's own retransmission timeout is ~31.8 s.
#define ACK_STALL_MS        10000
#define SUMMARY_PERIOD_MS   300000

static ntrip_params_t params;
static volatile bool started;
static volatile bool stop_requested;
static volatile bool pause_requested;
static volatile bool paused_ack;
static volatile uint32_t failures;
static rtcm_parser_t parser;

typedef enum { CONNECT_OK, CONNECT_RETRY_409, CONNECT_FAIL } connect_result_t;

static bool halted(void) {
    return stop_requested || pause_requested;
}

static void beat_sleep_ms(uint32_t ms) {
    // Sleep in slices so the core 1 heartbeat stays fresh.
    while (ms) {
        uint32_t step = ms > 1000 ? 1000 : ms;
        wdt_core1_heartbeat();
        sleep_ms(step);
        ms -= step;
        if (halted()) return;
    }
}

static void close_socket(void) {
    if (getSn_SR(SOCK_NTRIP) == SOCK_ESTABLISHED) wz_disconnect(SOCK_NTRIP);
    wz_close(SOCK_NTRIP);
    app_status.ntrip_connected = false;
}

static bool expired(absolute_time_t t) {
    return absolute_time_diff_us(get_absolute_time(), t) <= 0;
}

static connect_result_t connect_caster(void) {
    close_socket();
    printf("\n=== Connecting to NTRIP Caster ===\n");
    printf("Server: %s:%u\nMountpoint: %s\n", params.server, params.port, params.mountpoint);

    if (!net_is_up()) {
        printf("✗ Connection error: network down\n");
        return CONNECT_FAIL;
    }
    uint8_t ip[4];
    if (!net_resolve(params.server, ip)) {
        printf("✗ Connection error: DNS lookup failed\n");
        return CONNECT_FAIL;
    }
    printf("Resolved to: %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);

    uint16_t local_port = net_ephemeral_port();
    if (wz_socket(SOCK_NTRIP, Sn_MR_TCP, local_port, SF_IO_NONBLOCK) != SOCK_NTRIP) return CONNECT_FAIL;
    net_tcp_prepare(SOCK_NTRIP);
    printf("Local port: %u, MSS %u\n", local_port, net_tcp_mss());
    int8_t rc = wz_connect(SOCK_NTRIP, ip, params.port);
    if (rc != SOCK_OK && rc != SOCK_BUSY) {
        close_socket();
        return CONNECT_FAIL;
    }
    absolute_time_t deadline = make_timeout_time_ms(CONNECT_TIMEOUT_MS);
    while (getSn_SR(SOCK_NTRIP) != SOCK_ESTABLISHED) {
        if (getSn_SR(SOCK_NTRIP) == SOCK_CLOSED || expired(deadline)) {
            printf("✗ Connection error: TCP connect failed\n");
            close_socket();
            return CONNECT_FAIL;
        }
        wdt_core1_heartbeat();
        sleep_ms(5);
    }
    printf("✓ TCP connection established\n");
    net_note_internet_ok(ip);

    char cred[160], auth[220];
    snprintf(cred, sizeof(cred), "%s:%s", params.user, params.password);
    base64_encode((const uint8_t *)cred, strlen(cred), auth, sizeof(auth));
    memset(cred, 0, sizeof(cred));
    static char req[512];
    int n = snprintf(req, sizeof(req),
                     "POST /%s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Ntrip-Version: Ntrip/2.0\r\n"
                     "User-Agent: NTRIP RP2350 Base/" FW_VERSION_STRING "\r\n"
                     "Authorization: Basic %s\r\n"
                     "Connection: close\r\n\r\n",
                     params.mountpoint, params.server, auth);
    memset(auth, 0, sizeof(auth));
    printf("Sending source request for /%s\n", params.mountpoint);

    deadline = make_timeout_time_ms(RESPONSE_TIMEOUT_MS);
    int sent = 0;
    while (sent < n) {
        int32_t r = wz_send(SOCK_NTRIP, (uint8_t *)req + sent, (uint16_t)(n - sent));
        if (r == SOCK_BUSY && !expired(deadline)) {
            sleep_ms(1);
            continue;
        }
        if (r <= 0) {
            memset(req, 0, sizeof(req));
            printf("✗ Connection error: send failed\n");
            close_socket();
            return CONNECT_FAIL;
        }
        sent += r;
    }
    memset(req, 0, sizeof(req));

    char resp[257];
    size_t len = 0;
    while (!expired(deadline) && len < sizeof(resp) - 1) {
        uint16_t avail = getSn_RX_RSR(SOCK_NTRIP);
        if (avail) {
            int32_t r = wz_recv(SOCK_NTRIP, (uint8_t *)resp + len,
                             (uint16_t)(avail < sizeof(resp) - 1 - len ? avail : sizeof(resp) - 1 - len));
            if (r <= 0) break;
            len += (size_t)r;
            resp[len] = '\0';
            if (strstr(resp, "\r\n\r\n")) break;
        } else if (getSn_SR(SOCK_NTRIP) != SOCK_ESTABLISHED) {
            break;
        } else {
            wdt_core1_heartbeat();
            sleep_ms(5);
        }
    }
    resp[len] = '\0';
    char *eol = strpbrk(resp, "\r\n");
    printf("Received response: %.*s\n", eol ? (int)(eol - resp) : (int)len, resp);

    if (strstr(resp, "ICY 200 OK") || strstr(resp, "HTTP/1.1 200 OK") || strstr(resp, "HTTP/1.0 200 OK")) {
        printf("✓ Connected to NTRIP caster successfully\n");
        return CONNECT_OK;
    }
    close_socket();
    if (strstr(resp, " 409")) {
        printf("⚠ HTTP 409 Conflict — mountpoint busy, caster needs time to release\n");
        return CONNECT_RETRY_409;
    }
    printf("✗ Connection failed\n");
    return CONNECT_FAIL;
}

static void connect_with_retry(void) {
    uint32_t attempt = 0;
    while (!halted()) {
        attempt++;
        connect_result_t r = connect_caster();
        if (r == CONNECT_OK) {
            failures = 0;
            app_status.ntrip_connects++;
            app_status.ntrip_connected = true;
            return;
        }
        failures++;
        if (r == CONNECT_RETRY_409) {
            printf("Mountpoint busy — waiting %ds for caster to release... (attempt %lu)\n",
                   RETRY_DELAY_409_MS / 1000, (unsigned long)attempt);
            beat_sleep_ms(RETRY_DELAY_409_MS);
        } else {
            printf("Retrying in %ds... (attempt %lu)\n", RETRY_DELAY_MS / 1000, (unsigned long)attempt);
            beat_sleep_ms(RETRY_DELAY_MS);
        }
    }
}

static void log_stream_end(const char *why, uint32_t session_bytes) {
    printf("NTRIP stream ended: %s (sent %lu bytes this session, SR=0x%02x, TX free %u/%u, IR=0x%02x, port %u)\n", why,
           (unsigned long)session_bytes, getSn_SR(SOCK_NTRIP), getSn_TX_FSR(SOCK_NTRIP), getSn_TxMAX(SOCK_NTRIP),
           getSn_IR(SOCK_NTRIP), getSn_PORT(SOCK_NTRIP));
}

static void stream(void) {
    static uint8_t chunk[2048];
    uint32_t bytes_window = 0, session_bytes = 0;
    absolute_time_t next_summary = make_timeout_time_ms(SUMMARY_PERIOD_MS);
    uint32_t overruns = app_status.rtcm_overruns;
    const uint16_t tx_max = getSn_TxMAX(SOCK_NTRIP);
    uint16_t prev_free = getSn_TX_FSR(SOCK_NTRIP);
    absolute_time_t last_ack = get_absolute_time();

    // Whatever piled up while disconnected is stale.
    um980_data_flush();

    while (!halted()) {
        wdt_core1_heartbeat();

        if (expired(next_summary)) {
            printf("NTRIP: %lu bytes sent in last 5min\n", (unsigned long)bytes_window);
            bytes_window = 0;
            next_summary = make_timeout_time_ms(SUMMARY_PERIOD_MS);
        }

        uint8_t sr = getSn_SR(SOCK_NTRIP);
        if (sr != SOCK_ESTABLISHED) {
            bool timeout = getSn_IR(SOCK_NTRIP) & Sn_IR_TIMEOUT;
            log_stream_end(timeout ? "TCP retransmission timeout" : "connection closed", session_bytes);
            if (timeout && prev_free < tx_max && tx_max - prev_free > net_tcp_mss()) net_tcp_mss_step_down();
            return;
        }

        // TX free space only grows when the caster ACKs data: use it to
        // detect a dead path instead of waiting for the W5500 timeout.
        uint16_t space = getSn_TX_FSR(SOCK_NTRIP);
        if (space > prev_free || space == tx_max) last_ack = get_absolute_time();
        prev_free = space;
        if (space < tx_max && absolute_time_diff_us(last_ack, get_absolute_time()) > ACK_STALL_MS * 1000LL) {
            log_stream_end("no TCP ACK from caster for 10 s", session_bytes);
            // Unacknowledged segments may be too big for the path.
            if (tx_max - space > (int)net_tcp_mss()) net_tcp_mss_step_down();
            return;
        }

        size_t avail = um980_data_available(&overruns);
        if (overruns != app_status.rtcm_overruns) {
            printf("⚠ RTCM ring overrun, data dropped\n");
            app_status.rtcm_overruns = overruns;
        }
        if (!avail || !space) {
            sleep_ms(10);
            continue;
        }

        size_t n = avail;
        if (n > space) n = space;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        n = um980_data_peek(chunk, n);
        int32_t sent = wz_send(SOCK_NTRIP, chunk, (uint16_t)n);
        if (sent == SOCK_BUSY) {
            sleep_ms(1);
            continue;
        }
        if (sent <= 0) {
            char why[32];
            snprintf(why, sizeof(why), "send error %ld", (long)sent);
            log_stream_end(why, session_bytes);
            return;
        }
        prev_free = getSn_TX_FSR(SOCK_NTRIP);
        um980_data_consume((size_t)sent);
        rtcm_parser_feed(&parser, chunk, (size_t)sent);
        app_status.rtcm_frames = parser.frames;
        app_status.rtcm_crc_errors = parser.crc_errors;
        app_status.ntrip_bytes_total += (uint32_t)sent;
        bytes_window += (uint32_t)sent;
        session_bytes += (uint32_t)sent;
    }
}

static void core1_main(void) {
    flash_safe_execute_core_init();
    wdt_core1_heartbeat();
    printf("\n=== Starting NTRIP Thread ===\n");
    rtcm_parser_init(&parser);
    while (!stop_requested) {
        if (pause_requested) {
            if (!paused_ack) {
                close_socket();
                printf("NTRIP paused\n");
                paused_ack = true;
            }
            wdt_core1_heartbeat();
            sleep_ms(100);
            continue;
        }
        if (paused_ack) {
            paused_ack = false;
            printf("NTRIP resumed\n");
        }
        connect_with_retry();
        if (halted()) continue;
        stream();
        close_socket();
    }
    close_socket();
    printf("Disconnected from NTRIP caster\n");
    for (;;) {
        wdt_core1_heartbeat();
        sleep_ms(500);
    }
}

void ntrip_start(const ntrip_params_t *p) {
    params = *p;
    started = true;
    multicore_launch_core1(core1_main);
    // Flash writes from core 0 are only safe once core 1 can be locked out.
    absolute_time_t deadline = make_timeout_time_ms(1000);
    while (!multicore_lockout_victim_is_initialized(1) && !time_reached(deadline)) tight_loop_contents();
    printf("NTRIP thread started on core 1\n");
}

bool ntrip_started(void) { return started; }

uint32_t ntrip_failures(void) { return failures; }

bool ntrip_pause(uint32_t timeout_ms) {
    if (!started) return true;
    pause_requested = true;
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!paused_ack) {
        if (time_reached(deadline)) return false;
        idle_poll();
        sleep_ms(10);
    }
    return true;
}

void ntrip_resume(void) {
    pause_requested = false;
}

void ntrip_stop(void) {
    stop_requested = true;
}
