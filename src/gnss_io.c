#include "gnss_io.h"

#include <stdio.h>

#include "board.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "idle.h"
#include "pico/time.h"

static volatile uint32_t pps_count;
static volatile uint64_t pps_last_us;
static volatile uint32_t pps_period_us;
static volatile int32_t pps_dev_min, pps_dev_max;
static bool reset_ok;

static void pps_irq(void) {
    if (!(gpio_get_irq_event_mask(GNSS_PPS_PIN) & GPIO_IRQ_EDGE_RISE)) return;
    gpio_acknowledge_irq(GNSS_PPS_PIN, GPIO_IRQ_EDGE_RISE);
    uint64_t now = time_us_64();
    if (pps_last_us) {
        uint32_t period = (uint32_t)(now - pps_last_us);
        pps_period_us = period;
        // Ignore gaps (missed edges, e.g. IRQs masked during flash writes).
        if (period > 900000 && period < 1100000) {
            int32_t dev = (int32_t)period - 1000000;
            if (dev < pps_dev_min) pps_dev_min = dev;
            if (dev > pps_dev_max) pps_dev_max = dev;
        }
    }
    pps_last_us = now;
    pps_count++;
}

void gnss_io_init(void) {
    const uint pins[] = {GNSS_PPS_PIN, GNSS_RTK_STAT_PIN, GNSS_RESET_PIN, GNSS_EXTINT_PIN};
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_disable_pulls(pins[i]);
    }
    // The reset line is open-drain style: input normally, output-low to reset.
    gpio_put(GNSS_RESET_PIN, 0);
    sleep_us(100);
    int high = 0;
    for (int i = 0; i < 8; i++) {
        high += gpio_get(GNSS_RESET_PIN);
        sleep_us(50);
    }
    reset_ok = high == 8;
    printf("%s GNSS reset line %s\n", reset_ok ? "✓" : "⚠",
           reset_ok ? "OK (pulled up)" : "reads low: UM980 resets disabled");

    pps_reset_stats();
    gpio_add_raw_irq_handler(GNSS_PPS_PIN, pps_irq);
    gpio_set_irq_enabled(GNSS_PPS_PIN, GPIO_IRQ_EDGE_RISE, true);
    irq_set_enabled(IO_IRQ_BANK0, true);
}

void pps_get(pps_info_t *out) {
    uint32_t irq = save_and_disable_interrupts();
    out->count = pps_count;
    out->last_edge_us = pps_last_us;
    out->last_period_us = pps_period_us;
    out->dev_min_us = pps_dev_min;
    out->dev_max_us = pps_dev_max;
    restore_interrupts(irq);
}

void pps_reset_stats(void) {
    uint32_t irq = save_and_disable_interrupts();
    pps_dev_min = INT32_MAX;
    pps_dev_max = INT32_MIN;
    restore_interrupts(irq);
}

bool pps_fresh(void) {
    pps_info_t p;
    pps_get(&p);
    return p.count >= 2 && time_us_64() - p.last_edge_us < 1500000;
}

uint64_t pps_wait_edge(uint32_t timeout_ms) {
    uint32_t start = pps_count;
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (pps_count == start) {
        if (time_reached(deadline)) return 0;
        idle_poll();
        sleep_us(200);
    }
    pps_info_t p;
    pps_get(&p);
    return p.last_edge_us;
}

bool gnss_rtk_stat(void) {
    return gpio_get(GNSS_RTK_STAT_PIN);
}

bool gnss_reset_available(void) {
    return reset_ok;
}

bool gnss_reset_pulse(uint32_t ms) {
    if (!reset_ok) return false;
    printf("GNSS: resetting UM980 (%lu ms pulse)\n", (unsigned long)ms);
    gpio_put(GNSS_RESET_PIN, 0);
    gpio_set_dir(GNSS_RESET_PIN, GPIO_OUT);
    idle_sleep_ms(ms);
    gpio_set_dir(GNSS_RESET_PIN, GPIO_IN);  // release: the UM980 pulls it up
    return true;
}
