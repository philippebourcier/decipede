#include "net.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "dhcp.h"
#include "dns_proto.h"
#include "pico/rand.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "idle.h"
#include "pico/mutex.h"
#include "pico/rand.h"
#include "pico/time.h"
#include "pico/unique_id.h"
#include "socket.h"
#include "util.h"
#include "wiz.h"
#include "wdt.h"
#include "wizchip_conf.h"

#define DNS_CACHE_SIZE 4
#define DNS_CACHE_TTL_MS (10 * 60 * 1000)

static recursive_mutex_t spi_mutex;
static mutex_t dns_mutex;
static repeating_timer_t tick_timer;
static uint8_t dhcp_buf[1024];
static bool use_dhcp;
static bool have_ip;

// Static IP -> DHCP fallback: with no Internet success for 5 min in static
// mode, try DHCP; if no lease comes within 2 min, go back to the static
// settings and only try again 30 min later.
#define FALLBACK_AFTER_S 300
#define FALLBACK_LEASE_S 120
#define FALLBACK_RETRY_S 1800
typedef enum { FB_NONE, FB_TRYING, FB_ACTIVE } fallback_t;
static wiz_NetInfo static_ni;
static bool static_valid;
static uint32_t static_since_s, fb_started_s, fb_retry_after_s;
static fallback_t fb_state;
static volatile uint32_t last_inet_ok_s;
static uint8_t mac[6];

static struct {
    char host[64];
    uint8_t ip[4];
    uint32_t at_ms;
} dns_cache[DNS_CACHE_SIZE];

// --- ioLibrary platform callbacks -------------------------------------------

static void cs_select(void) { gpio_put(W5500_CS_PIN, 0); }
static void cs_deselect(void) { gpio_put(W5500_CS_PIN, 1); }

static uint8_t spi_read_byte(void) {
    uint8_t b;
    spi_read_blocking(W5500_SPI, 0x00, &b, 1);
    return b;
}

static void spi_write_byte(uint8_t b) {
    spi_write_blocking(W5500_SPI, &b, 1);
}

static void spi_read_burst(uint8_t *buf, uint16_t len) {
    spi_read_blocking(W5500_SPI, 0x00, buf, len);
}

static void spi_write_burst(uint8_t *buf, uint16_t len) {
    spi_write_blocking(W5500_SPI, buf, len);
}

void net_lock(void) { recursive_mutex_enter_blocking(&spi_mutex); }
void net_unlock(void) { recursive_mutex_exit(&spi_mutex); }

static bool tick_cb(repeating_timer_t *t) {
    DHCP_time_handler();
    return true;
}

// Locally administered MAC hashed from the full 64-bit chip ID (see
// mac_from_unique_id in util.c).
static void make_mac(uint8_t out[6]) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    mac_from_unique_id(id.id, out);
}

bool net_hw_init(void) {
    recursive_mutex_init(&spi_mutex);
    mutex_init(&dns_mutex);

    spi_init(W5500_SPI, W5500_SPI_HZ);
    gpio_set_function(W5500_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(W5500_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(W5500_MISO_PIN, GPIO_FUNC_SPI);
    gpio_init(W5500_CS_PIN);
    gpio_set_dir(W5500_CS_PIN, GPIO_OUT);
    gpio_put(W5500_CS_PIN, 1);
    gpio_init(W5500_RST_PIN);
    gpio_set_dir(W5500_RST_PIN, GPIO_OUT);

    gpio_put(W5500_RST_PIN, 0);
    sleep_ms(2);
    gpio_put(W5500_RST_PIN, 1);
    sleep_ms(10);

    reg_wizchip_cris_cbfunc(net_lock, net_unlock);
    reg_wizchip_cs_cbfunc(cs_select, cs_deselect);
    reg_wizchip_spi_cbfunc(spi_read_byte, spi_write_byte);
    reg_wizchip_spiburst_cbfunc(spi_read_burst, spi_write_burst);

    // TX/RX buffer KB per socket (16 KB each in total): NTRIP gets a large
    // TX buffer, the HTTP client a large RX buffer for OTA downloads; DHCP,
    // DNS and NTP datagrams fit in 1 KB.
    //                DHCP DNS NTRIP HTTPD HTTPC NTP HTTPD2 spare
    uint8_t tx[8] = {1,   1,  8,    2,    1,    1,  2,     0};
    uint8_t rx[8] = {1,   1,  1,    2,    8,    1,  2,     0};
    if (wizchip_init(tx, rx) != 0) {
        printf("✗ W5500 buffer init failed\n");
        return false;
    }
    uint8_t ver = getVERSIONR();
    if (ver != 0x04) {
        printf("✗ W5500 not found (VERSIONR=0x%02X)\n", ver);
        return false;
    }

    make_mac(mac);
    setSHAR(mac);
    add_repeating_timer_ms(1000, tick_cb, NULL, &tick_timer);
    printf("✓ W5500 detected\n");
    return true;
}

static void on_ip_assign(void) {
    wiz_NetInfo ni;
    wizchip_getnetinfo(&ni);
    getIPfromDHCP(ni.ip);
    getGWfromDHCP(ni.gw);
    getSNfromDHCP(ni.sn);
    getDNSfromDHCP(ni.dns);
    ni.dhcp = NETINFO_DHCP;
    wizchip_setnetinfo(&ni);
    have_ip = true;
}

static void on_ip_conflict(void) {
    printf("✗ DHCP: IP conflict\n");
    have_ip = false;
}

bool net_start(bool dhcp, const char *ip, const char *subnet, const char *gateway,
               const char *dns, uint32_t timeout_s) {
    printf("\n=== Initializing W5x00 Ethernet ===\n");
    use_dhcp = dhcp;
    have_ip = false;

    wiz_NetInfo ni;
    memset(&ni, 0, sizeof(ni));
    memcpy(ni.mac, mac, 6);
    if (!dhcp && (!parse_ipv4(ip, ni.ip) || !parse_ipv4(subnet, ni.sn) || !parse_ipv4(gateway, ni.gw) ||
                  !parse_ipv4(dns, ni.dns))) {
        printf("⚠ Invalid static IP configuration, using DHCP\n");
        dhcp = true;
        use_dhcp = true;
        memset(&ni, 0, sizeof(ni));
        memcpy(ni.mac, mac, 6);
    }
    if (dhcp) {
        printf("Using DHCP...\n");
        ni.dhcp = NETINFO_DHCP;
        wizchip_setnetinfo(&ni);
        reg_dhcp_cbfunc(on_ip_assign, on_ip_assign, on_ip_conflict);
        net_lock();
        DHCP_init(SOCK_DHCP, dhcp_buf);
        net_unlock();
    } else {
        printf("Using static IP: %s\n", ip);
        ni.dhcp = NETINFO_STATIC;
        wizchip_setnetinfo(&ni);
        have_ip = true;
        static_ni = ni;
        static_valid = true;
        static_since_s = uptime_s();
        fb_state = FB_NONE;
    }
    printf("MAC Address: %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    printf("Waiting for Ethernet connection");
    absolute_time_t deadline = make_timeout_time_ms(timeout_s * 1000);
    absolute_time_t next_dot = make_timeout_time_ms(500);
    while (!net_is_up()) {
        net_poll();
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
            printf("\n✗ Ethernet connection timeout\n");
            return false;
        }
        if (absolute_time_diff_us(get_absolute_time(), next_dot) <= 0) {
            printf(".");
            next_dot = make_timeout_time_ms(500);
        }
        idle_sleep_ms(10);
    }
    printf("\n✓ Ethernet connected\n");
    return true;
}

void net_note_internet_ok(const uint8_t peer[4]) {
    // Only hosts beyond the local subnet prove the gateway works: a LAN
    // server stays reachable with a wrong gateway.
    wiz_NetInfo ni;
    wizchip_getnetinfo(&ni);
    for (int i = 0; i < 4; i++) {
        if ((peer[i] & ni.sn[i]) != (ni.ip[i] & ni.sn[i])) {
            last_inet_ok_s = uptime_s();
            return;
        }
    }
}

static void start_dhcp_fallback(void) {
    printf("⚠ Static IP: no Internet for %d min, trying DHCP\n", FALLBACK_AFTER_S / 60);
    wiz_NetInfo ni;
    memset(&ni, 0, sizeof(ni));
    memcpy(ni.mac, mac, 6);
    ni.dhcp = NETINFO_DHCP;
    wizchip_setnetinfo(&ni);
    have_ip = false;
    use_dhcp = true;
    reg_dhcp_cbfunc(on_ip_assign, on_ip_assign, on_ip_conflict);
    net_lock();
    DHCP_init(SOCK_DHCP, dhcp_buf);
    net_unlock();
    fb_state = FB_TRYING;
    fb_started_s = uptime_s();
}

static void back_to_static(void) {
    printf("⚠ DHCP fallback: no lease, back to the static IP (next try in %d min)\n", FALLBACK_RETRY_S / 60);
    net_lock();
    close(SOCK_DHCP);
    net_unlock();
    use_dhcp = false;
    wizchip_setnetinfo(&static_ni);
    have_ip = true;
    static_since_s = uptime_s();
    fb_retry_after_s = static_since_s + FALLBACK_RETRY_S;
    fb_state = FB_NONE;
}

void net_poll(void) {
    uint32_t now = uptime_s();
    if (!use_dhcp) {
        uint32_t ok = last_inet_ok_s > static_since_s ? last_inet_ok_s : static_since_s;
        if (static_valid && now >= fb_retry_after_s && now - ok > FALLBACK_AFTER_S) start_dhcp_fallback();
        return;
    }
    if (wizphy_getphylink() != PHY_LINK_ON) return;
    net_lock();
    uint8_t rc = DHCP_run();
    net_unlock();
    if (rc == DHCP_FAILED) {
        have_ip = false;
    } else if (rc == DHCP_IP_LEASED || rc == DHCP_IP_ASSIGN || rc == DHCP_IP_CHANGED) {
        have_ip = true;
    }
    if (fb_state == FB_TRYING) {
        if (have_ip) {
            fb_state = FB_ACTIVE;
            printf("✓ DHCP fallback: lease obtained (static settings kept for next boot)\n");
            net_print_status();
        } else if (now - fb_started_s > FALLBACK_LEASE_S) {
            back_to_static();
        }
    }
}

const char *net_mode(void) {
    if (fb_state == FB_ACTIVE) return "DHCP (static fallback)";
    if (fb_state == FB_TRYING) return "trying DHCP (static fallback)";
    return use_dhcp ? "DHCP" : "static";
}

bool net_is_up(void) {
    return have_ip && wizphy_getphylink() == PHY_LINK_ON;
}

void net_get_info(net_info_t *info) {
    wiz_NetInfo ni;
    wizchip_getnetinfo(&ni);
    info->link = wizphy_getphylink() == PHY_LINK_ON;
    info->has_ip = have_ip;
    memcpy(info->mac, ni.mac, 6);
    memcpy(info->ip, ni.ip, 4);
    memcpy(info->subnet, ni.sn, 4);
    memcpy(info->gateway, ni.gw, 4);
    memcpy(info->dns, ni.dns, 4);
}

void net_print_status(void) {
    net_info_t ni;
    net_get_info(&ni);
    printf("\n=== Network Status ===\n");
    printf("Connected: %s\n", ni.link && ni.has_ip ? "True" : "False");
    if (ni.link && ni.has_ip) {
        printf("IP:      %d.%d.%d.%d\n", ni.ip[0], ni.ip[1], ni.ip[2], ni.ip[3]);
        printf("Subnet:  %d.%d.%d.%d\n", ni.subnet[0], ni.subnet[1], ni.subnet[2], ni.subnet[3]);
        printf("Gateway: %d.%d.%d.%d\n", ni.gateway[0], ni.gateway[1], ni.gateway[2], ni.gateway[3]);
        printf("DNS:     %d.%d.%d.%d\n", ni.dns[0], ni.dns[1], ni.dns[2], ni.dns[3]);
    }
}

static const uint16_t mss_ladder[] = {1460, 1360, 1200, 536};
#define MSS_STEPS (sizeof(mss_ladder) / sizeof(mss_ladder[0]))
static volatile uint16_t tcp_mss = 1460;
static volatile uint16_t tcp_mss_fixed;  // non-zero: configured, never probed

void net_tcp_set_mss_limit(uint16_t mss) {
    tcp_mss_fixed = mss;
    if (mss) tcp_mss = mss < 536 ? 536 : mss > 1460 ? 1460 : mss;
}

uint16_t net_tcp_mss(void) {
    return tcp_mss;
}

void net_tcp_prepare(uint8_t sn) {
    net_lock();
    setSn_MSSR(sn, tcp_mss);
    setSn_FRAG(sn, 0x0000);  // clear Don't-Fragment (reset value is 0x4000)
    net_unlock();
}

bool net_tcp_mss_step_down(void) {
    if (tcp_mss_fixed) return false;
    for (size_t i = 0; i + 1 < MSS_STEPS; i++) {
        if (tcp_mss >= mss_ladder[i] && tcp_mss > mss_ladder[i + 1]) {
            tcp_mss = mss_ladder[i + 1];
            printf("⚠ TCP: possible MTU black hole, lowering MSS to %u\n", tcp_mss);
            return true;
        }
    }
    return false;
}

uint16_t net_ephemeral_port(void) {
    return (uint16_t)(49152u + get_rand_32() % (65536u - 49152u));
}

// --- Non-blocking UDP send ------------------------------------------------------
// ioLibrary's sendto() spins until the datagram is sent or ARP times out
// (seconds, for an unreachable address), whatever the socket mode. These
// drive the W5500 directly and only hold the lock per register access.

bool net_udp_send_start(uint8_t sn, const uint8_t *buf, uint16_t len, const uint8_t ip[4], uint16_t port) {
    net_lock();
    bool ok = getSn_TX_FSR(sn) >= len;
    if (ok) {
        setSn_DIPR(sn, (uint8_t *)ip);
        setSn_DPORT(sn, port);
        setSn_IR(sn, (Sn_IR_SENDOK | Sn_IR_TIMEOUT));
        wiz_send_data(sn, (uint8_t *)buf, len);
        setSn_CR(sn, Sn_CR_SEND);
        while (getSn_CR(sn)) {
        }
    }
    net_unlock();
    return ok;
}

int net_udp_send_poll(uint8_t sn) {
    net_lock();
    uint8_t ir = getSn_IR(sn);
    int r = 0;
    if (ir & Sn_IR_SENDOK) r = 1;
    else if (ir & Sn_IR_TIMEOUT) r = -1;  // ARP failed
    if (r) setSn_IR(sn, (Sn_IR_SENDOK | Sn_IR_TIMEOUT));
    net_unlock();
    return r;
}

// Wait without starving anything: core 0 keeps idle work and the watchdog
// going, core 1 keeps its heartbeat.
static void net_wait_step(bool core0) {
    if (core0) idle_poll();
    else wdt_core1_heartbeat();
    sleep_ms(2);
}

#define DNS_TRIES 2
#define DNS_SEND_TIMEOUT_MS 3000
#define DNS_REPLY_TIMEOUT_MS 2500

static bool dns_lookup(const uint8_t server[4], const char *host, uint8_t ip[4], bool core0) {
    static uint8_t msg[512];
    for (int attempt = 0; attempt < DNS_TRIES; attempt++) {
        uint16_t id = (uint16_t)get_rand_32();
        size_t qlen = dns_build_query(id, host, msg, sizeof(msg));
        if (!qlen) return false;
        if (wz_socket(SOCK_DNS, Sn_MR_UDP, net_ephemeral_port(), SF_BROAD_BLOCK) != SOCK_DNS) return false;
        bool ok = false;
        if (net_udp_send_start(SOCK_DNS, msg, (uint16_t)qlen, server, 53)) {
            absolute_time_t deadline = make_timeout_time_ms(DNS_SEND_TIMEOUT_MS);
            int sent = 0;
            while (!(sent = net_udp_send_poll(SOCK_DNS)) && !time_reached(deadline)) net_wait_step(core0);
            if (sent == 1) {
                deadline = make_timeout_time_ms(DNS_REPLY_TIMEOUT_MS);
                while (!ok && !time_reached(deadline)) {
                    if (getSn_RX_RSR(SOCK_DNS) > 0) {
                        uint8_t from[4];
                        uint16_t port;
                        int32_t n = wz_recvfrom(SOCK_DNS, msg, sizeof(msg), from, &port);
                        if (n > 0 && port == 53) ok = dns_parse_reply(id, msg, (size_t)n, ip);
                    } else {
                        net_wait_step(core0);
                    }
                }
            }
        }
        wz_close(SOCK_DNS);
        if (ok) return true;
    }
    return false;
}

bool net_resolve(const char *host, uint8_t ip[4]) {
    if (parse_ipv4(host, ip)) return true;
    if (strlen(host) >= sizeof(dns_cache[0].host)) return false;

    // Core 0 must keep the watchdog fed while core 1 holds the resolver.
    bool core0 = get_core_num() == 0;
    while (!mutex_try_enter(&dns_mutex, NULL)) {
        if (core0) idle_poll();
        sleep_ms(5);
    }

    uint32_t now = to_ms_since_boot(get_absolute_time());
    bool ok = false;
    int slot = 0;
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (dns_cache[i].host[0] && strcmp(dns_cache[i].host, host) == 0 &&
            now - dns_cache[i].at_ms < DNS_CACHE_TTL_MS) {
            memcpy(ip, dns_cache[i].ip, 4);
            ok = true;
            break;
        }
        if (dns_cache[i].at_ms < dns_cache[slot].at_ms) slot = i;
    }

    if (!ok) {
        wiz_NetInfo ni;
        wizchip_getnetinfo(&ni);
        ok = dns_lookup(ni.dns, host, ip, core0);
        if (ok) {
            str_copy(dns_cache[slot].host, sizeof(dns_cache[slot].host), host);
            memcpy(dns_cache[slot].ip, ip, 4);
            dns_cache[slot].at_ms = to_ms_since_boot(get_absolute_time());
        } else {
            printf("ERROR: DNS lookup failed for %s\n", host);
        }
    }
    mutex_exit(&dns_mutex);
    return ok;
}
