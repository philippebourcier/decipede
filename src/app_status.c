#include "app_status.h"

#include "pico/time.h"

app_status_t app_status = {
    .led1 = COLOR_ORANGE,
    .led2 = COLOR_ORANGE,
    .led2_base = COLOR_ORANGE,
    .boot_partition = -1,
};

void status_set_led1(color_t c) {
    app_status.led1 = c;
    led_set(LED1, c);
}

static repeating_timer_t blink_timer;
static volatile bool blink_lit;

static bool blink_cb(repeating_timer_t *t) {
    blink_lit = !blink_lit;
    led_set(LED2, blink_lit ? COLOR_RED : COLOR_OFF);
    return true;
}

static bool alarm_shown(void) {
    // Solid red (receiver failure) wins over the blinking PPS alarm.
    return app_status.led2_blink && app_status.led2_base != COLOR_RED;
}

static void apply_led2(void) {
    cancel_repeating_timer(&blink_timer);
    if (alarm_shown()) {
        app_status.led2 = COLOR_RED;
        blink_lit = true;
        led_set(LED2, COLOR_RED);
        add_repeating_timer_ms(500, blink_cb, NULL, &blink_timer);
    } else {
        app_status.led2 = app_status.led2_base;
        led_set(LED2, app_status.led2);
    }
}

void status_set_led2(color_t c) {
    bool was = alarm_shown();
    app_status.led2_base = c;
    if (was && alarm_shown()) return;  // keep blinking; c shows once cleared
    apply_led2();
}

void status_set_pps_alarm(bool on) {
    if (on == app_status.led2_blink) return;
    app_status.led2_blink = on;
    apply_led2();
}

void status_restore_leds(void) {
    led_set(LED1, app_status.led1);
    apply_led2();
}
