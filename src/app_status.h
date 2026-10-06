// Live status shared by the main loop, NTRIP core and status web page
// (the Python `status` dict).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gnss_logic.h"
#include "led.h"

typedef struct {
    bool valid;
    float value;
} opt_float_t;

typedef struct {
    color_t led1;
    color_t led2;        // colour shown (RED while the PPS alarm blinks)
    color_t led2_base;   // colour requested by the regular checks
    bool led2_blink;     // PPS alarm: LED2 blinks red
    opt_float_t temperature;
    opt_float_t humidity;
    opt_float_t agc_l1, agc_l2, agc_l5;
    opt_float_t rms_max_delta;
    opt_float_t pitch_delta;
    opt_float_t roll_delta;
    opt_float_t pitch, roll;  // last IMU tilt, degrees
    bool vibrating;
    bool level_valid;
    bool level;
    opt_float_t mcu_temp;     // RP2350 internal sensor, deg C
    bool sats_valid;
    sats_t sats;              // last SATSINFOA
    bool pps_dev_valid;       // PPS period deviation over the last minute, us
    int32_t pps_dev_min_us, pps_dev_max_us;
    char um980_model[24];
    char um980_firmware[48];
    // Written by core 1
    volatile bool ntrip_connected;
    volatile uint32_t ntrip_bytes_total;
    volatile uint32_t rtcm_frames;
    volatile uint32_t rtcm_crc_errors;
    volatile uint32_t rtcm_overruns;
    volatile uint32_t ntrip_connects;
    // OTA / boot
    int boot_partition;
    char ota_state[48];
} app_status_t;

extern app_status_t app_status;

static inline opt_float_t opt_some(float v) { return (opt_float_t){true, v}; }
static inline opt_float_t opt_none(void) { return (opt_float_t){false, 0.0f}; }

void status_set_led1(color_t c);
void status_set_led2(color_t c);
// No-PPS alarm: LED2 blinks red (1 Hz) and overrides the other LED2 colours
// (except solid red, which keeps showing a receiver failure). Clearing it
// restores the last requested colour.
void status_set_pps_alarm(bool on);
// Re-apply the recorded colours (after the button blink).
void status_restore_leds(void);
