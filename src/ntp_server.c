#include "ntp_server.h"

#include <stdio.h>

#include "board.h"
#include "config.h"
#include "gnss_io.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "net.h"
#include "ntp_proto.h"
#include "pico/time.h"
#include "utc.h"
#include "wiz.h"

#define NTP_PORT 123

static bool open_;
static bool send_pending;  // previous reply still in flight (ARP)
static absolute_time_t send_started;
static uint32_t requests;
static uint32_t dropped;  // queued behind another packet: arrival time unknown
static volatile uint64_t rx_irq_us;  // last W5500 interrupt (falling edge)

static void w5500_int_irq(void) {
    if (!(gpio_get_irq_event_mask(W5500_INT_PIN) & GPIO_IRQ_EDGE_FALL)) return;
    gpio_acknowledge_irq(W5500_INT_PIN, GPIO_IRQ_EDGE_FALL);
    rx_irq_us = time_us_64();
}

void ntp_server_init(void) {
    gpio_init(W5500_INT_PIN);
    gpio_set_dir(W5500_INT_PIN, GPIO_IN);
    gpio_pull_up(W5500_INT_PIN);
    gpio_add_raw_irq_handler(W5500_INT_PIN, w5500_int_irq);
    gpio_set_irq_enabled(W5500_INT_PIN, GPIO_IRQ_EDGE_FALL, true);
    irq_set_enabled(IO_IRQ_BANK0, true);
}

static void set_open(bool want) {
    if (want == open_) return;
    net_lock();
    if (want) {
        // Broadcast datagrams (LAN chatter) are dropped by the W5500 itself.
        if (socket(SOCK_NTP, Sn_MR_UDP, NTP_PORT, SF_BROAD_BLOCK) == SOCK_NTP) {
            // Only the NTP socket's RECV event drives the INTn line.
            setSn_IMR(SOCK_NTP, Sn_IR_RECV);
            setSIMR(getSIMR() | (1u << SOCK_NTP));
            setSn_IR(SOCK_NTP, 0x1F);
            open_ = true;
        }
    } else {
        setSIMR(getSIMR() & (uint8_t)~(1u << SOCK_NTP));
        close(SOCK_NTP);
        open_ = false;
    }
    net_unlock();
    printf("NTP server %s\n", open_ ? "enabled (UDP 123)" : "disabled");
}

void ntp_server_poll(void) {
    set_open(config.ntp_server && net_is_up());
    if (!open_) return;

    for (int n = 0; n < 32 && getSn_RX_RSR(SOCK_NTP) > 0; n++) {
        // The arrival time is only known for the packet that raised the
        // interrupt. Take it, then clear the interrupt: anything still
        // queued arrived at an unknown time and is dropped below rather
        // than answered with a wrong timestamp (clients simply retry).
        uint64_t irq = rx_irq_us;
        rx_irq_us = 0;
        uint8_t req[64], ip[4], reply[NTP_PACKET_SIZE];
        uint16_t port;
        int32_t len = wz_recvfrom(SOCK_NTP, req, sizeof(req), ip, &port);
        net_lock();
        setSn_IR(SOCK_NTP, Sn_IR_RECV);  // the next arrival raises INTn again
        net_unlock();
        if (len <= 0) break;
        if (!irq || time_us_64() - irq > 200000) {
            dropped++;
            continue;
        }
        uint64_t rx_us = irq;

        ntp_times_t t = {0};
        int64_t ref_s;
        t.synced = pps_fresh() && utc_at_us(rx_us, &t.rx_unix_us) && utc_last_label(&ref_s);
        if (t.synced) t.ref_unix_us = ref_s * 1000000;
        else t.rx_unix_us = 0;
        uint64_t tx_us = time_us_64();
        if (t.synced && !utc_at_us(tx_us, &t.tx_unix_us)) t.synced = false;
        if (!t.synced) t.tx_unix_us = 0;  // LI=3 / stratum 16: clients ignore us
        if (!ntp_build_reply(req, (size_t)len, &t, reply)) continue;
        // Non-blocking send. The W5500 can't start a new SEND while the
        // previous one waits for ARP (vanished client): drop this reply then.
        if (send_pending && !net_udp_send_poll(SOCK_NTP) && !time_reached(send_started)) continue;
        send_pending = net_udp_send_start(SOCK_NTP, reply, NTP_PACKET_SIZE, ip, port);
        send_started = make_timeout_time_ms(3000);
        requests++;
    }
}

uint32_t ntp_server_dropped(void) {
    return dropped;
}

uint32_t ntp_server_requests(void) {
    return requests;
}
