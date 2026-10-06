// RTK base station firmware for the W5500-EVB-Pico2 + UM980.
// C port of uPyRTKBase (base.py): same boot sequence, LED codes, status
// page and telemetry, plus A/B OTA updates.
//
// Order of operations:
//  1. Load local config (determines DHCP vs static)
//  2. Bring up the W5500, accept the trial image (OTA stage 1)
//  3. Initialize Ethernet, download remote config
//  4. Initialize UM980
//  5. Initialize IMU (LSM6DSV16X) and environment sensor (SHT40)
//  6. Launch NTRIP on core 1 (COM2 -> caster)
//  7. Main loop: IMU check, AGC check, periodic telemetry + summary, OTA,
//     status page on port 80, BTN_USER hold > 3 s = reboot

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_status.h"
#include "board.h"
#include "config.h"
#include "cpu_load.h"
#include "mcu_temp.h"
#include "gnss_io.h"
#include "gnss_logic.h"
#include "gnss_watchdog.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/watchdog.h"
#include "http_client.h"
#include "http_server.h"
#include "idle.h"
#include "led.h"
#include "lsm6dsv.h"
#include "net.h"
#include "ntp_server.h"
#include "ntrip.h"
#include "ota.h"
#include "pico/stdlib.h"
#include "sht4x.h"
#include "stack_mon.h"
#include "um980.h"
#include "um980_fw.h"
#include "utc.h"
#include "util.h"
#include "wdt.h"

#define REBOOT_HOLD_MS     3000
#define SAMPLE_PERIOD_MS   60000
#define SUMMARY_PERIOD_MS  300000
#define UM980_RETRY_MS     60000
#define NET_RETRY_MS       30000
#define NET_LINK_TIMEOUT_S 120
#define BUF_SAMPLES        5

// --- Button (non-blocking): hold > 3 s to reboot ---------------------------

static bool btn_held;
static absolute_time_t btn_pressed_at;

static void check_button(void) {
    if (gpio_get(BTN_USER_PIN)) {
        if (!btn_held) {
            btn_held = true;
            btn_pressed_at = get_absolute_time();
            printf("Button held...\n");
            return;
        }
        int64_t held_ms = absolute_time_diff_us(btn_pressed_at, get_absolute_time()) / 1000;
        led_both((held_ms / 200) % 2 == 0 ? COLOR_WHITE : COLOR_OFF);
        if (held_ms >= REBOOT_HOLD_MS) {
            printf("Button held >3s — rebooting...\n");
            led_both(COLOR_WHITE);
            sleep_ms(300);
            led_off();
            ntrip_stop();
            watchdog_reboot(0, 0, 10);
            for (;;) tight_loop_contents();
        }
    } else if (btn_held) {
        printf("Button released after %lldms — ignoring\n",
               absolute_time_diff_us(btn_pressed_at, get_absolute_time()) / 1000);
        btn_held = false;
        status_restore_leds();
    }
}

// Background work while core 0 is busy elsewhere.
static bool background_net;

static void background(void) {
    cpu_load_tick();
    check_button();
    if (background_net) {
        net_poll();
        http_server_poll();
        ntp_server_poll();
    }
}

// --- Telemetry (fire-and-forget) --------------------------------------------

static int json_opt(char *buf, size_t size, size_t pos, const char *key, opt_float_t v, int decimals) {
    if (pos >= size) return (int)size;
    int n = v.valid ? snprintf(buf + pos, size - pos, ",\"%s\":%.*f", key, decimals, (double)v.value)
                    : snprintf(buf + pos, size - pos, ",\"%s\":null", key);
    return (int)pos + (n > 0 ? n : 0);
}

static void send_telemetry(opt_float_t temp, opt_float_t hum, opt_float_t l1, opt_float_t l2, opt_float_t l5) {
    if (!config.telemetry_url[0]) return;
    char body[512];
    int pos = snprintf(body, sizeof(body), "{\"hw\":\"%s\",\"fw\":\"%s\"", hardware_id(), FW_VERSION_STRING);
    pos = json_opt(body, sizeof(body), (size_t)pos, "rms_max_delta", app_status.rms_max_delta, 4);
    pos = json_opt(body, sizeof(body), (size_t)pos, "pitch_delta", app_status.pitch_delta, 4);
    pos = json_opt(body, sizeof(body), (size_t)pos, "roll_delta", app_status.roll_delta, 4);
    pos = json_opt(body, sizeof(body), (size_t)pos, "temperature", temp, 2);
    pos = json_opt(body, sizeof(body), (size_t)pos, "humidity", hum, 2);
    pos = json_opt(body, sizeof(body), (size_t)pos, "agc_l1", l1, 1);
    pos = json_opt(body, sizeof(body), (size_t)pos, "agc_l2", l2, 1);
    pos = json_opt(body, sizeof(body), (size_t)pos, "agc_l5", l5, 1);
    char ts[32];
    utc_now_iso(ts, sizeof(ts));
    if ((size_t)pos < sizeof(body))
        pos += snprintf(body + pos, sizeof(body) - (size_t)pos,
                        ",\"ts\":%s%s%s,\"pps\":%s,\"rtk_stat\":%d,\"um980_fw\":\"%s\",\"gnss_resets\":%lu",
                        ts[0] == '-' ? "" : "\"", ts[0] == '-' ? "null" : ts, ts[0] == '-' ? "" : "\"",
                        pps_fresh() ? "true" : "false", gnss_rtk_stat(), app_status.um980_firmware,
                        (unsigned long)gnss_watchdog_resets());
    if ((size_t)pos + 2 >= sizeof(body)) return;
    body[pos++] = '}';
    body[pos] = '\0';
    if (http_post_json(config.telemetry_url, body, 5000)) printf("✓ Telemetry sent\n");
}

// --- IMU min/max tracking over the 5-minute window --------------------------

typedef struct {
    bool valid;
    float min, max;
} range_t;

static void range_add(range_t *r, float v) {
    if (!r->valid) {
        r->valid = true;
        r->min = r->max = v;
    } else {
        if (v < r->min) r->min = v;
        if (v > r->max) r->max = v;
    }
}

static opt_float_t range_delta(const range_t *r) {
    return r->valid ? opt_some(r->max - r->min) : opt_none();
}

// Query the UM980 satellite list (dashboard counts and sky plot).
static void update_sats(void) {
    uint64_t tag_us;
    const char *r = um980_query_log("SATSINFOA", "#SATSINFOA", 2500, &tag_us);
    app_status.sats_valid = r && satsinfo_parse(r, &app_status.sats);
    if (app_status.sats_valid) printf("Satellites tracked: %u\n", app_status.sats.total);
}

// --- Network bring-up ---------------------------------------------------------

static bool network_up(void) {
    net_tcp_set_mss_limit(config.tcp_mss > 0 ? (uint16_t)config.tcp_mss : 0);
    bool ok = net_start(config.dhcp, config.ip, config.subnet, config.gateway, config.dns, NET_LINK_TIMEOUT_S);
    if (ok) {
        background_net = true;
        net_print_status();
    }
    return ok;
}

static void fetch_config(void) {
    printf("\n[STEP 3] Downloading Remote Configuration...\n");
    printf("Hardware ID: %s\n", hardware_id());
    if (config_download()) {
        printf("✓ Configuration downloaded\n");
        config_print();
        status_set_led1(COLOR_GREEN);
        ota_mark_healthy();
    } else {
        printf("⚠ Using local configuration\n");
        status_set_led1(COLOR_YELLOW);
    }
}

static bool start_um980(void) {
    printf("\n[STEP 4] Initializing UM980 Sensor...\n");
    status_set_led2(COLOR_YELLOW);
    if (!um980_start(app_status.um980_model, sizeof(app_status.um980_model), app_status.um980_firmware,
                     sizeof(app_status.um980_firmware))) {
        printf("✗ Failed to initialize UM980\n");
        status_set_led2(COLOR_RED);
        return false;
    }
    printf("✓ UM980 ready: %s, FW: %s\n", app_status.um980_model, app_status.um980_firmware);
    status_set_led2(COLOR_CYAN);
    return true;
}

static void start_ntrip(void) {
    printf("\n[STEP 6] Starting NTRIP Caster Thread...\n");
    if (!config.ntrip_server[0] || !config.ntrip_mountpoint[0] || !config.ntrip_user[0] ||
        !config.ntrip_password[0]) {
        printf("⚠ Missing NTRIP configuration — skipping\n");
        printf("  Required: ntrip_server, ntrip_mountpoint, ntrip_user, ntrip_password\n");
        return;
    }
    ntrip_params_t p;
    memset(&p, 0, sizeof(p));
    str_copy(p.server, sizeof(p.server), config.ntrip_server);
    p.port = (uint16_t)config.ntrip_port;
    str_copy(p.mountpoint, sizeof(p.mountpoint), config.ntrip_mountpoint);
    str_copy(p.user, sizeof(p.user), config.ntrip_user);
    str_copy(p.password, sizeof(p.password), config.ntrip_password);
    ntrip_start(&p);
    printf("✓ NTRIP thread started on core 1\n");
}

static float avg(const float *v, int n) {
    float s = 0;
    for (int i = 0; i < n; i++) s += v[i];
    return s / (float)n;
}

// Average AGC samples, skipping -1 (unknown).
static opt_float_t avg_agc(const agc_values_t *buf, int n, int band) {
    float s = 0;
    int k = 0;
    for (int i = 0; i < n; i++) {
        int v = band == 0 ? buf[i].l1 : band == 1 ? buf[i].l2 : buf[i].l5;
        if (v != -1) {
            s += (float)v;
            k++;
        }
    }
    return k ? opt_some(s / (float)k) : opt_none();
}

static int app_main(void);

int main(void) {
    // Don't let the pad's default pull-down hold the UM980 in reset.
    gpio_disable_pulls(GNSS_RESET_PIN);
    stack_run(app_main);  // core 0 continues on a 16 KB stack
}

static int app_main(void) {
    // Don't let the pad's default pull-down hold the UM980 in reset.
    gpio_disable_pulls(GNSS_RESET_PIN);
    stdio_init_all();
    led_init();
    status_set_led1(COLOR_ORANGE);
    status_set_led2(COLOR_ORANGE);
    gpio_init(BTN_USER_PIN);
    gpio_set_dir(BTN_USER_PIN, GPIO_IN);
    gpio_disable_pulls(BTN_USER_PIN);  // external 10K pull-down
    idle_set_handler(background);
    sleep_ms(1500);  // let the USB console enumerate

    printf("============================================================\n");
    printf("RTK BASE STATION  firmware %s\n", FW_VERSION_STRING);
    printf("============================================================\n");
    if (wdt_caused_reboot()) printf("⚠ Previous reset was caused by the watchdog\n");

    ota_boot_init();

    printf("\n[STEP 1] Loading Local Configuration...\n");
    config_set_defaults();
    config_load_local();

    i2c_init(SENSOR_I2C, SENSOR_I2C_HZ);
    gpio_set_function(SENSOR_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SENSOR_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(SENSOR_I2C_SDA_PIN);
    gpio_pull_up(SENSOR_I2C_SCL_PIN);
    um980_uart_init();
    gnss_io_init();

    printf("\n[STEP 2] Initializing Ethernet...\n");
    bool w5500_ok = net_hw_init();
    // OTA stage 1: the image boots, reads its config and talks to the
    // W5500. Accept it now, well within the bootrom's ~16.7 s window.
    if (w5500_ok) ota_buy();
    if (!ota_buy_pending()) {
        wdt_start();
    } else {
        // Not feeding a watchdog lets the bootrom's TBYB timer expire and
        // boot the previous image.
        printf("✗ Trial image not accepted, the bootrom will roll back\n");
    }

    bool net_ok = w5500_ok && network_up();
    if (!net_ok) {
        printf("✗ Failed to initialize network\n");
        status_set_led1(COLOR_RED);
    } else {
        status_set_led1(COLOR_CYAN);
        fetch_config();
    }

    bool um980_ok = start_um980();

    printf("\n[STEP 5] Initializing Sensors...\n");
    static lsm6dsv_t imu_dev = {
        .addr = 0x6B, .tilt_threshold_deg = 2.0f, .vibration_threshold_dps = 0.5f, .vibration_window_ms = 1000};
    lsm6dsv_t *imu = NULL;
    if (lsm6dsv_init(&imu_dev)) {
        imu = &imu_dev;
        printf("✓ LSM6DSV16X ready\n");
    } else {
        printf("⚠ LSM6DSV16X init failed\n");
    }
    float t, h;
    if (sht40_read(&t, &h)) {
        printf("✓ SHT40 ready: %.1fC  %.1f%%\n", (double)t, (double)h);
        // Shown on the status page straight away (the regular sampling starts after 60 s).
        app_status.temperature = opt_some(t);
        app_status.humidity = opt_some(h);
    }
    else printf("⚠ SHT40 read failed\n");

    if (um980_ok && net_ok) start_ntrip();

    gnss_watchdog_init();
    gnss_fw_init();
    ntp_server_init();
    if (um980_ok) {
        printf("\n[STEP 6b] UTC from PPS...\n");
        char iso[32];
        if (utc_sync()) {
            utc_now_iso(iso, sizeof(iso));
            printf("✓ UTC %s\n", iso);
        } else {
            printf("⚠ UTC not available yet (PPS: %s)\n", pps_fresh() ? "yes" : "no");
        }
    }

    printf("\n[STEP 7] HTTP status server on port 80\n");
    printf("\n[STEP 8] Main Loop Running...\n");
    printf("============================================================\n");
    printf("Base station is operational!\n");
    printf("============================================================\n");

    float temp_buf[BUF_SAMPLES], hum_buf[BUF_SAMPLES];
    int env_n = 0;
    agc_values_t agc_buf[BUF_SAMPLES];
    int agc_n = 0;
    range_t r_rms = {0}, r_pitch = {0}, r_roll = {0};

    absolute_time_t next_sample = make_timeout_time_ms(SAMPLE_PERIOD_MS);
    absolute_time_t next_summary = make_timeout_time_ms(SUMMARY_PERIOD_MS);
    absolute_time_t next_hw_retry = make_timeout_time_ms(net_ok ? UM980_RETRY_MS : NET_RETRY_MS);
    bool ntrip_was_connected = false;

    // First MCU temperature and satellite readings now rather than after the
    // first 60 s period, so the status page has them right after boot. (AGC
    // stays on the regular cycle: it drives LED2 and the GNSS watchdog.)
    app_status.mcu_temp = opt_some(mcu_temp_read());
    if (um980_ok) update_sats();

    for (;;) {
        idle_poll();
        if (background_net) {
            net_poll();
            http_server_poll();
            ntp_server_poll();
        }

        // --- Recover from boot-time failures --------------------------------
        if ((!net_ok || !um980_ok) && time_reached(next_hw_retry)) {
            next_hw_retry = make_timeout_time_ms(net_ok ? UM980_RETRY_MS : NET_RETRY_MS);
            if (!net_ok && w5500_ok) {
                net_ok = network_up();
                if (net_ok) {
                    status_set_led1(COLOR_CYAN);
                    fetch_config();
                }
            }
            if (!um980_ok) um980_ok = start_um980();
            if (um980_ok && net_ok && !ntrip_started()) start_ntrip();
        }

        // --- No-PPS alarm on LED2 (blinking red) ------------------------------
        // After 60 s without PPS (no satellite lock) while the receiver is up;
        // cold starts lock well within that. Cleared as soon as PPS returns.
        {
            static absolute_time_t pps_lost_since;
            bool alarm = false;
            if (um980_ok && !gnss_fw_busy() && !pps_fresh()) {
                if (is_nil_time(pps_lost_since)) pps_lost_since = get_absolute_time();
                alarm = absolute_time_diff_us(pps_lost_since, get_absolute_time()) >= 60 * 1000000LL;
            } else {
                pps_lost_since = nil_time;
            }
            if (alarm != app_status.led2_blink) printf("%s No PPS alarm %s\n", alarm ? "⚠" : "✓", alarm ? "on" : "cleared");
            status_set_pps_alarm(alarm);
        }

        // --- LED1 follows the NTRIP connection --------------------------------
        if (ntrip_started()) {
            bool connected = app_status.ntrip_connected;
            if (connected) {
                if (!ntrip_was_connected) status_set_led1(COLOR_GREEN);
                ota_mark_healthy();
            } else if (ntrip_was_connected) {
                printf("⚠ NTRIP disconnected, reconnecting...\n");
                status_set_led1(COLOR_ORANGE);
            } else if (ntrip_failures() >= 3 && app_status.led1 != COLOR_RED) {
                printf("✗ NTRIP reconnect failing\n");
                status_set_led1(COLOR_RED);
            }
            ntrip_was_connected = connected;
        }

        // --- IMU check -> LED2 ----------------------------------------------------
        if (imu) {
            imu_check_t c;
            if (lsm6dsv_check(imu, &c)) {
                range_add(&r_rms, c.rms_max_dps);
                if (c.level_valid) {
                    range_add(&r_pitch, c.pitch_deg);
                    range_add(&r_roll, c.roll_deg);
                }
                if (c.level_valid) {
                    app_status.pitch = opt_some(c.pitch_deg);
                    app_status.roll = opt_some(c.roll_deg);
                }
                app_status.vibrating = c.vibrating;
                app_status.level_valid = c.level_valid;
                app_status.level = c.level;
                app_status.rms_max_delta = range_delta(&r_rms);
                app_status.pitch_delta = range_delta(&r_pitch);
                app_status.roll_delta = range_delta(&r_roll);
                if (c.vibrating || (c.level_valid && !c.level)) {
                    status_set_led2(COLOR_PINK);
                    if (c.vibrating) printf("⚠ Vibration: rms=%.3f dps\n", (double)c.rms_max_dps);
                    else printf("⚠ Tilt: pitch=%.2f  roll=%.2f\n", (double)c.pitch_deg, (double)c.roll_deg);
                }
            } else {
                printf("✗ IMU check error\n");
            }
        } else {
            idle_sleep_ms(1000);
        }

        // --- Every 60 s: SHT40 + AGC -> LED2 --------------------------------------
        if (time_reached(next_sample)) {
            next_sample = make_timeout_time_ms(SAMPLE_PERIOD_MS);

            if (sht40_read(&t, &h)) {
                if (env_n == BUF_SAMPLES) {
                    memmove(temp_buf, temp_buf + 1, sizeof(float) * (BUF_SAMPLES - 1));
                    memmove(hum_buf, hum_buf + 1, sizeof(float) * (BUF_SAMPLES - 1));
                    env_n--;
                }
                temp_buf[env_n] = t;
                hum_buf[env_n] = h;
                env_n++;
                app_status.temperature = opt_some(t);
                app_status.humidity = opt_some(h);
            } else {
                printf("⚠ SHT40 sample error\n");
            }

            agc_values_t agc;
            bool agc_ok = um980_ok && um980_get_agc(&agc);
            if (agc_ok) {
                if (agc_n == BUF_SAMPLES) {
                    memmove(agc_buf, agc_buf + 1, sizeof(agc_values_t) * (BUF_SAMPLES - 1));
                    agc_n--;
                }
                agc_buf[agc_n++] = agc;
                app_status.agc_l1 = opt_some((float)agc.l1);
                app_status.agc_l2 = opt_some((float)agc.l2);
                app_status.agc_l5 = opt_some((float)agc.l5);

                printf("\n=== AGC Status ===\n");
                const int vals[3] = {agc.l1, agc.l2, agc.l5};
                const char *names[3] = {"L1", "L2", "L5"};
                bool bad = false;
                for (int i = 0; i < 3; i++) {
                    agc_state_t s = um980_agc_state(vals[i]);
                    printf("%s: %s (value: %d)\n", names[i],
                           s == AGC_GOOD ? "good" : s == AGC_BAD ? "bad" : "unknown", vals[i]);
                    bad |= s == AGC_BAD;
                }
                // Don't override an IMU alarm seen during this window.
                bool antenna_ok = !imu || !((r_rms.valid && r_rms.max > 0.5f) || !r_pitch.valid);
                if (bad) {
                    printf("⚠ Poor AGC\n");
                    if (antenna_ok) status_set_led2(COLOR_BLUE);
                } else {
                    printf("✓ AGC status good\n");
                    if (antenna_ok) status_set_led2(COLOR_GREEN);
                }
            } else if (um980_ok) {
                printf("⚠ AGC sample error\n");
                status_set_led2(COLOR_RED);
            }

            // Satellites in view (dashboard sky plot), MCU temperature.
            if (um980_ok) update_sats();
            app_status.mcu_temp = opt_some(mcu_temp_read());

            // UTC label from PPS + RECTIMEA, then the GNSS watchdog.
            bool utc_ok = utc_sync();
            pps_info_t pps;
            pps_get(&pps);
            if (pps.dev_min_us <= pps.dev_max_us)
                printf("PPS: %lu edges, period dev %ld..%ld us, UTC %s\n", (unsigned long)pps.count,
                       (long)pps.dev_min_us, (long)pps.dev_max_us, utc_ok ? "synced" : "not synced");
            app_status.pps_dev_valid = pps.dev_min_us <= pps.dev_max_us;
            app_status.pps_dev_min_us = pps.dev_min_us;
            app_status.pps_dev_max_us = pps.dev_max_us;
            pps_reset_stats();
            bool com1_ok = utc_last_query_answered() || agc_ok;
            switch (gnss_watchdog_minute(com1_ok)) {
            case WDOG_RESET:
                gnss_reset_pulse(500);
                idle_sleep_ms(3000);
                um980_ok = start_um980();
                break;
            case WDOG_RECONFIGURE:
                um980_ok = start_um980();
                break;
            default:
                break;
            }
        }

        // --- Every 5 min: telemetry + summary ---------------------------------------
        if (time_reached(next_summary)) {
            next_summary = make_timeout_time_ms(SUMMARY_PERIOD_MS);
            printf("\n=== 5 Minute Summary ===\n");
            net_print_status();

            opt_float_t avg_t = env_n ? opt_some(avg(temp_buf, env_n)) : opt_none();
            opt_float_t avg_h = env_n ? opt_some(avg(hum_buf, env_n)) : opt_none();
            if (avg_t.valid) printf("Temp: %.1fC  Humidity: %.1f%%\n", (double)avg_t.value, (double)avg_h.value);
            printf("NTRIP connected: %s  (%lu bytes total, %lu RTCM frames, %lu CRC errors)\n",
                   app_status.ntrip_connected ? "True" : "False", (unsigned long)app_status.ntrip_bytes_total,
                   (unsigned long)app_status.rtcm_frames, (unsigned long)app_status.rtcm_crc_errors);

            app_status.rms_max_delta = range_delta(&r_rms);
            app_status.pitch_delta = range_delta(&r_pitch);
            app_status.roll_delta = range_delta(&r_roll);
            if (net_ok) {
                send_telemetry(avg_t, avg_h, avg_agc(agc_buf, agc_n, 0), avg_agc(agc_buf, agc_n, 1),
                               avg_agc(agc_buf, agc_n, 2));
            }
            memset(&r_rms, 0, sizeof(r_rms));
            memset(&r_pitch, 0, sizeof(r_pitch));
            memset(&r_roll, 0, sizeof(r_roll));
        }

        ota_poll();
        gnss_fw_poll(&um980_ok);
    }
    return 0;  // not reached
}
