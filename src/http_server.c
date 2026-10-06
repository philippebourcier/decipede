#include "http_server.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_status.h"
#include "build_config.h"
#include "config.h"
#include "hardware/clocks.h"
#include "cpu_load.h"
#include "net.h"
#include "gnss_io.h"
#include "gnss_watchdog.h"
#include "ntp_server.h"
#include "ota.h"
#include "um980.h"
#include "um980_fw.h"
#include "utc.h"
#include "pico/time.h"
#include "socket.h"
#include "stack_mon.h"
#include "wiz.h"
#include "util.h"

#define HTTP_PORT 80
#define REQUEST_TIMEOUT_MS 2000
#define SEND_TIMEOUT_MS 3000

static char page[8192];
static size_t page_len;
// Two listening sockets on port 80, so a second browser isn't refused while
// the first is served. Each has its own connection state.
static const uint8_t server_socks[] = {SOCK_HTTPD, SOCK_HTTPD2};
#define NSERVERS (sizeof(server_socks) / sizeof(server_socks[0]))
static absolute_time_t conn_deadline[NSERVERS];
static bool conn_active[NSERVERS];
static absolute_time_t closing_since[NSERVERS];
static uint8_t cur_sn;  // socket being served

static void out(const char *fmt, ...) {
    if (page_len >= sizeof(page)) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(page + page_len, sizeof(page) - page_len, fmt, ap);
    va_end(ap);
    if (n > 0) page_len += (size_t)n;
    if (page_len > sizeof(page)) page_len = sizeof(page);
}

static const struct {
    color_t color;
    const char *css;
    const char *led1_meaning;
    const char *led2_meaning;
} led_meta[] = {
    {COLOR_ORANGE, "#f97316", "Booting / NTRIP reconnecting", "Booting"},
    {COLOR_CYAN, "#22d3ee", "Ethernet up, downloading config", "UM980 up, checks pending"},
    {COLOR_GREEN, "#22c55e", "Config downloaded, NTRIP connected", "AGC good, level, no vibration"},
    {COLOR_YELLOW, "#eab308", "Ethernet up, remote config failed (using local)", "UM980 initializing"},
    {COLOR_RED, "#ef4444", "Ethernet init failed / NTRIP reconnect failed", "UM980 init failed / AGC check error"},
    {COLOR_BLUE, "#3b82f6", "Unknown", "AGC degraded on one or more bands"},
    {COLOR_PINK, "#ec4899", "Unknown", "Vibrating or tilted (IMU alarm)"},
};

static void led_indicator(const char *label, color_t c, bool led2) {
    const char *css = "#444", *meaning = "Unknown";
    if (led2 && app_status.led2 == COLOR_RED && app_status.led2_blink) {
        out("\n    <div class=\"led-block\">\n"
            "      <div class=\"led-label\">%s</div>\n"
            "      <div class=\"led-circle blink\" style=\"background:#ef4444;box-shadow:0 0 24px 8px #ef4444\"></div>\n"
            "      <div class=\"led-meaning\">Blinking: no PPS, no satellite lock (antenna/sky?)</div>\n"
            "    </div>",
            label);
        return;
    }
    for (size_t i = 0; i < sizeof(led_meta) / sizeof(led_meta[0]); i++) {
        if (led_meta[i].color == c) {
            css = led_meta[i].css;
            meaning = led2 ? led_meta[i].led2_meaning : led_meta[i].led1_meaning;
        }
    }
    out("\n    <div class=\"led-block\">\n"
        "      <div class=\"led-label\">%s</div>\n"
        "      <div class=\"led-circle\" style=\"background:%s;box-shadow:0 0 24px 8px %s\"></div>\n"
        "      <div class=\"led-meaning\">%s</div>\n"
        "    </div>",
        label, css, css, meaning);
}

static void row(const char *label, const char *value) {
    out("<tr><td>%s</td><td><b>%s</b></td></tr>", label, value);
}

static void row_opt(const char *label, opt_float_t v, const char *unit, int decimals) {
    char buf[32];
    if (!v.valid) str_copy(buf, sizeof(buf), "-");
    else snprintf(buf, sizeof(buf), "%.*f%s", decimals, (double)v.value, unit);
    row(label, buf);
}

static void html_escape(const char *value, char *buf, size_t size);

static void html_escape_text(const char *value) {
    char buf[96];
    html_escape(value, buf, sizeof(buf));
    out("%s", buf);
}

static void html_escape_row(const char *label, const char *value) {
    char buf[96];
    html_escape(value, buf, sizeof(buf));
    row(label, buf[0] ? buf : "-");
}

static void html_escape(const char *value, char *buf, size_t size) {
    size_t o = 0;
    for (; *value && o + 7 < size; value++) {
        const char *rep = NULL;
        switch (*value) {
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '&': rep = "&amp;"; break;
        case '"': rep = "&quot;"; break;
        default: break;
        }
        if (rep) {
            size_t n = strlen(rep);
            memcpy(buf + o, rep, n);
            o += n;
        } else {
            buf[o++] = *value;
        }
    }
    buf[o] = '\0';
}

static void build_html(void) {
    const app_status_t *s = &app_status;
    char tmp[64];
    page_len = 0;
    out("<!DOCTYPE html>\n<html>\n<head>\n"
        "  <meta charset=\"utf-8\">\n"
        "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "  <title>" BRAND_NAME " base station</title>\n"
        "  <style>\n"
        "    body        { font-family: monospace; background: #111; color: #eee; padding: 2em; margin: 0; }\n"
        "    .container  { max-width: 480px; margin: 0 auto; }\n"
        "    h1          { color: #7cf; margin-bottom: 0.5em; }\n"
        "    .leds       { display: flex; gap: 3em; margin-bottom: 2em; }\n"
        "    .led-block  { display: flex; flex-direction: column; align-items: center; gap: 0.4em; }\n"
        "    .led-label  { font-size: 0.85em; color: #aaa; text-align: center; }\n"
        "    .led-circle { width: 64px; height: 64px; border-radius: 50%%; }\n"
        "    .led-meaning{ font-size: 0.8em; color: #ccc; text-align: center; max-width: 160px; }\n"
        "    .blink      { animation: blink 1s step-start infinite; }\n"
        "    @keyframes blink { 50%% { opacity: 0.15; } }\n"
        "    table       { border-collapse: collapse; width: 100%%; }\n"
        "    td          { padding: 6px 16px; border-bottom: 1px solid #333; }\n"
        "    td:first-child { color: #aaa; }\n"
        "    .footer     { color: #555; font-size: 0.8em; margin-top: 1.5em; }\n"
        "    .fw         { color: #aaa; margin: -0.4em 0 1.5em 0; }\n"
        "    .fw b       { color: #7cf; }\n"
        "    .ota        { margin-top: 1.5em; }\n"
        "    .ota button { font-family: monospace; font-size: 1em; padding: 0.6em 1.2em; background: #1e3a5f;\n"
        "                  color: #eee; border: 1px solid #7cf; border-radius: 4px; cursor: pointer; }\n"
        "    .ota button:hover { background: #2b5486; }\n"
        "    .ota .state { color: #aaa; font-size: 0.85em; margin-top: 0.6em; }\n"
        "  </style>\n</head>\n<body>\n  <div class=\"container\">\n"
        "    <h1>" BRAND_NAME "</h1>\n"
        "    <p class=\"fw\">Firmware <b>v%s</b> &middot; partition %c</p>\n"
        "    <div class=\"leds\">",
        FW_VERSION_STRING, s->boot_partition == 0 ? 'A' : s->boot_partition == 1 ? 'B' : '-');
    led_indicator("LED1 - Internet / NTRIP", s->led1, false);
    led_indicator("LED2 - GNSS / Antenna", s->led2, true);
    out("\n    </div>\n    <table>");

    row("Hardware ID", hardware_id());
    row("NTRIP", s->ntrip_connected ? "Connected" : "Disconnected");
    html_escape_row("Mountpoint", config.ntrip_mountpoint);
    row_opt("Temperature", s->temperature, " C", 1);
    row_opt("Humidity", s->humidity, " %", 1);
    row_opt("AGC L1", s->agc_l1, "", 1);
    row_opt("AGC L2", s->agc_l2, "", 1);
    row_opt("AGC L5", s->agc_l5, "", 1);
    row_opt("RMS max d", s->rms_max_delta, " dps", 3);
    row_opt("Pitch d", s->pitch_delta, " deg", 2);
    row_opt("Roll d", s->roll_delta, " deg", 2);
    row("Vibrating", s->vibrating ? "YES" : "No");
    row("Level", !s->level_valid ? "-" : s->level ? "Yes" : "NO");
    snprintf(tmp, sizeof(tmp), "%lu frames, %lu CRC err", (unsigned long)s->rtcm_frames,
             (unsigned long)s->rtcm_crc_errors);
    row("RTCM", tmp);
    char utc[32];
    utc_now_iso(utc, sizeof(utc));
    row("UTC", utc);
    pps_info_t pps;
    pps_get(&pps);
    if (pps_fresh()) snprintf(tmp, sizeof(tmp), "OK (%lu pulses, last %lu us)", (unsigned long)pps.count,
                              (unsigned long)pps.last_period_us);
    else snprintf(tmp, sizeof(tmp), "none");
    row("PPS", tmp);
    row("RTK_STAT", gnss_rtk_stat() ? "1 (RTK fixed)" : "0");
    char fw[272];
    snprintf(fw, sizeof(fw), "%s %s", s->um980_model, s->um980_firmware);
    html_escape_row("UM980", fw);
    snprintf(tmp, sizeof(tmp), "%s, %lu/3 resets", gnss_watchdog_state(), (unsigned long)gnss_watchdog_resets());
    html_escape_row("GNSS watchdog", tmp);
    html_escape_row("UM980 FW update", gnss_fw_state());
    row("Network", net_mode());
    row("Config", config_source());
    if (config.ntp_server) snprintf(tmp, sizeof(tmp), "on, %lu requests", (unsigned long)ntp_server_requests());
    else snprintf(tmp, sizeof(tmp), "off");
    row("NTP server", tmp);
    snprintf(tmp, sizeof(tmp), "core 0 %u.%u %%, core 1 %u.%u %% (peak %u.%u / %u.%u %%)", cpu_load_permille(0) / 10,
             cpu_load_permille(0) % 10, cpu_load_permille(1) / 10, cpu_load_permille(1) % 10,
             cpu_load_peak_permille(0) / 10, cpu_load_peak_permille(0) % 10, cpu_load_peak_permille(1) / 10,
             cpu_load_peak_permille(1) % 10);
    row("CPU", tmp);
    row("Firmware", FW_VERSION_STRING);
    snprintf(tmp, sizeof(tmp), "%c", s->boot_partition == 0 ? 'A' : s->boot_partition == 1 ? 'B' : '-');
    row("Partition", tmp);
    html_escape_row("OTA", s->ota_state);
    uint32_t up = uptime_s();
    snprintf(tmp, sizeof(tmp), "%lud %02lu:%02lu:%02lu", (unsigned long)(up / 86400), (unsigned long)(up / 3600 % 24),
             (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));
    row("Uptime", tmp);

    out("</table>\n");
    if (config.firmware_url[0]) {
        out("    <form class=\"ota\" method=\"POST\" action=\"/ota/check\">\n"
            "      <button type=\"submit\">Check for firmware update</button>\n"
            "      <div class=\"state\">");
        html_escape_text(s->ota_state);
        out("</div>\n    </form>\n");
    } else {
        out("    <p class=\"ota state\">Firmware updates disabled (no firmware_url configured)</p>\n");
    }
    out("    <p class=\"footer\">Hold BTN_USER &gt;3s to reboot &middot; <a href=\"/\" style=\"color:#7cf\">Dashboard</a></p>\n  </div>\n</body>\n</html>");
}

static void json_opt(const char *key, opt_float_t v, bool comma) {
    if (v.valid) out("\"%s\":%.4f%s", key, (double)v.value, comma ? "," : "");
    else out("\"%s\":null%s", key, comma ? "," : "");
}

// JSON string value (escaped).
static void json_str(const char *key, const char *v, bool comma) {
    out("\"%s\":\"", key);
    for (; *v && page_len < sizeof(page) - 8; v++) {
        unsigned char c = (unsigned char)*v;
        if (c == '"' || c == '\\') out("\\%c", c);
        else if (c < 0x20) out("\\u%04x", c);
        else out("%c", c);
    }
    out("\"%s", comma ? "," : "");
}

static const char *led_meaning(color_t c, bool led2) {
    if (led2 && app_status.led2_blink && c == COLOR_RED) return "Blinking: no PPS, no satellite lock (antenna/sky?)";
    for (size_t i = 0; i < sizeof(led_meta) / sizeof(led_meta[0]); i++)
        if (led_meta[i].color == c) return led2 ? led_meta[i].led2_meaning : led_meta[i].led1_meaning;
    return "Unknown";
}

static const char *agc_state_name(opt_float_t v) {
    if (!v.valid) return "unknown";
    agc_state_t st = um980_agc_state((int)v.value);
    return st == AGC_GOOD ? "good" : st == AGC_BAD ? "bad" : "unknown";
}

// Dashboard additions: base, network, LEDs, tilt, MCU, timing, satellites.
static void build_json_extra(void) {
    const app_status_t *s = &app_status;
    json_str("base_mode", config.base_mode, true);
    out("\"base_lat\":%.9f,\"base_lon\":%.9f,\"base_alt\":%.3f,", config.base_lat, config.base_lon, config.base_alt);
    json_str("mountpoint", config.ntrip_mountpoint, true);
    json_str("ntrip_server", config.ntrip_server, true);
    json_str("ota", s->ota_state, true);
    out("\"ota_enabled\":%s,", config.firmware_url[0] ? "true" : "false");
    json_str("um980_model", s->um980_model, true);
    json_str("led1_meaning", led_meaning(s->led1, false), true);
    json_str("led2_meaning", led_meaning(s->led2, true), true);
    out("\"led2_blink\":%s,", s->led2_blink && s->led2 == COLOR_RED ? "true" : "false");
    net_info_t ni;
    net_get_info(&ni);
    out("\"ip\":\"%u.%u.%u.%u\",\"gateway\":\"%u.%u.%u.%u\",", ni.ip[0], ni.ip[1], ni.ip[2], ni.ip[3],
        ni.gateway[0], ni.gateway[1], ni.gateway[2], ni.gateway[3]);
    out("\"agc_state\":[\"%s\",\"%s\",\"%s\"],", agc_state_name(s->agc_l1), agc_state_name(s->agc_l2),
        agc_state_name(s->agc_l5));
    json_opt("pitch", s->pitch, true);
    json_opt("roll", s->roll, true);
    json_opt("mcu_temp", s->mcu_temp, true);
    out("\"mcu_mhz\":%lu,", (unsigned long)(clock_get_hz(clk_sys) / 1000000));
    if (s->pps_dev_valid) out("\"pps_dev_us\":[%ld,%ld],", (long)s->pps_dev_min_us, (long)s->pps_dev_max_us);
    else out("\"pps_dev_us\":null,");
    if (!s->sats_valid) {
        out("\"sats\":null,\"sky\":[],");
        return;
    }
    out("\"sats\":{\"total\":%u", s->sats.total);
    for (unsigned i = 0; i < SAT_NSYS; i++)
        if (s->sats.per_sys[i]) out(",\"%s\":%u", sat_sys_name(i), s->sats.per_sys[i]);
    out("},\"sky\":[");
    for (unsigned i = 0; i < s->sats.n; i++) {
        const sat_info_t *si = &s->sats.sat[i];
        out("%s[%u,%u,%u,%u,%u]", i ? "," : "", si->sys, si->prn, si->az, si->el, si->cn0);
    }
    out("],");
}

static void build_json(void) {
    const app_status_t *s = &app_status;
    page_len = 0;
    out("{\"hw\":\"%s\",\"fw\":\"%s\",\"partition\":%d,\"uptime\":%lu,\"led1\":\"%s\",\"led2\":\"%s\","
        "\"ntrip\":%s,\"ntrip_bytes\":%lu,\"rtcm_frames\":%lu,\"rtcm_crc_errors\":%lu,",
        hardware_id(), FW_VERSION_STRING, s->boot_partition, (unsigned long)uptime_s(), color_name(s->led1),
        s->led2_blink && s->led2 == COLOR_RED ? "RED_BLINK" : color_name(s->led2), s->ntrip_connected ? "true" : "false", (unsigned long)s->ntrip_bytes_total,
        (unsigned long)s->rtcm_frames, (unsigned long)s->rtcm_crc_errors);
    json_opt("temperature", s->temperature, true);
    json_opt("humidity", s->humidity, true);
    json_opt("agc_l1", s->agc_l1, true);
    json_opt("agc_l2", s->agc_l2, true);
    json_opt("agc_l5", s->agc_l5, true);
    json_opt("rms_max_delta", s->rms_max_delta, true);
    json_opt("pitch_delta", s->pitch_delta, true);
    json_opt("roll_delta", s->roll_delta, true);
    out("\"vibrating\":%s,\"level\":%s,\"rtcm_overruns\":%lu,\"tcp_mss\":%u,", s->vibrating ? "true" : "false",
        !s->level_valid ? "null" : s->level ? "true" : "false", (unsigned long)s->rtcm_overruns, net_tcp_mss());
    char utc[32];
    utc_now_iso(utc, sizeof(utc));
    pps_info_t pps;
    pps_get(&pps);
    out("\"utc\":%s%s%s,\"pps\":%s,\"pps_count\":%lu,\"pps_period_us\":%lu,\"rtk_stat\":%d,",
        utc[0] == '-' ? "" : "\"", utc[0] == '-' ? "null" : utc, utc[0] == '-' ? "" : "\"",
        pps_fresh() ? "true" : "false", (unsigned long)pps.count, (unsigned long)pps.last_period_us, gnss_rtk_stat());
    json_str("um980_fw", s->um980_firmware, true);
    json_str("gnss_watchdog", gnss_watchdog_state(), true);
    out("\"gnss_resets\":%lu,", (unsigned long)gnss_watchdog_resets());
    json_str("gnss_fw", gnss_fw_state(), true);
    build_json_extra();
    out("\"cpu\":[%u.%u,%u.%u],\"cpu_peak\":[%u.%u,%u.%u],", cpu_load_permille(0) / 10, cpu_load_permille(0) % 10,
        cpu_load_permille(1) / 10, cpu_load_permille(1) % 10, cpu_load_peak_permille(0) / 10,
        cpu_load_peak_permille(0) % 10, cpu_load_peak_permille(1) / 10, cpu_load_peak_permille(1) % 10);
    out("\"stack_free\":%lu,\"config_source\":\"%s\",", (unsigned long)stack_free_min(), config_source());
    out("\"net_mode\":\"%s\",\"ntp_server\":%s,\"ntp_requests\":%lu,\"ntp_dropped\":%lu,\"sockets\":\"", net_mode(),
        config.ntp_server ? "true" : "false", (unsigned long)ntp_server_requests(), (unsigned long)ntp_server_dropped());
    for (uint8_t sn = 0; sn < 8; sn++) out("%s%02x", sn ? " " : "", getSn_SR(sn));
    out("\"}");
}

static void send_all(const char *data, size_t len) {
    absolute_time_t deadline = make_timeout_time_ms(SEND_TIMEOUT_MS);
    while (len) {
        uint16_t chunk = len > 1024 ? 1024 : (uint16_t)len;
        int32_t n = wz_send(cur_sn, (uint8_t *)data, chunk);
        if (n == SOCK_BUSY) {
            if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
                printf("[HTTP] Send stalled\n");
                return;
            }
            sleep_ms(1);
            continue;
        }
        if (n <= 0) return;
        data += n;
        len -= (size_t)n;
    }
}

static void respond(const char *status, const char *type, const char *body, size_t len) {
    char hdr[160];
    int n = snprintf(hdr, sizeof(hdr), "HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
                     status, type, (unsigned)len);
    send_all(hdr, (size_t)n);
    if (len) send_all(body, len);
}

// Dashboard page, generated from web/index.html by tools/build_web.py.
extern const uint8_t web_index_gz[];
extern const size_t web_index_gz_len;
extern const char web_index_etag[];

static void send_dashboard(const char *headers) {
    char hdr[256];
    int n;
    // If-None-Match carrying our ETag: the browser's copy is current.
    char inm[64] = "";
    const char *h = headers ? str_icase_find(headers, "\nif-none-match:") : NULL;
    if (h) str_ncopy(inm, sizeof(inm), h + 15, strcspn(h + 15, "\r\n"));
    if (inm[0] && strstr(inm, web_index_etag)) {
        n = snprintf(hdr, sizeof(hdr), "HTTP/1.0 304 Not Modified\r\nETag: %s\r\nConnection: close\r\n\r\n", web_index_etag);
        send_all(hdr, (size_t)n);
        return;
    }
    n = snprintf(hdr, sizeof(hdr),
                 "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Encoding: gzip\r\n"
                 "Content-Length: %u\r\nCache-Control: no-cache\r\nETag: %s\r\nConnection: close\r\n\r\n",
                 (unsigned)web_index_gz_len, web_index_etag);
    send_all(hdr, (size_t)n);
    send_all((const char *)web_index_gz, web_index_gz_len);
}

static void handle_request(void) {
    static uint8_t req[1024];
    uint16_t avail = getSn_RX_RSR(cur_sn);
    int32_t n = wz_recv(cur_sn, req, avail >= sizeof(req) ? sizeof(req) - 1 : avail);
    if (n <= 0) return;
    req[n] = '\0';
    char *eol = strpbrk((char *)req, "\r\n");
    const char *headers = NULL;
    if (eol) {
        *eol = '\0';
        headers = eol + 1;  // "\n<Header>: ...\r\n..." (searched for "\n<name>:")
    }
    printf("[HTTP] Request: %.80s\n", (char *)req);

    if (!str_starts_with((char *)req, "GET ") && !str_starts_with((char *)req, "POST /ota/check")) {
        // Garbage or unsupported method: answer cheaply.
        respond("400 Bad Request", "text/plain", "", 0);
    } else if (str_starts_with((char *)req, "POST /ota/check")) {
        printf("[HTTP] OTA check requested\n");
        // UM980 first (flashed right away if newer), then the RP2350 check,
        // which ends in a reboot when there is an update (see um980_fw.c).
        gnss_fw_request_check();
        if (str_starts_with((char *)req, "POST /ota/check?js")) {
            respond("204 No Content", "text/plain", "", 0);  // dashboard fetch()
        } else {
            // /basic form: post/redirect/get, so a refresh doesn't repeat the POST.
            const char *redirect =
                "HTTP/1.0 303 See Other\r\nLocation: /basic\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send_all(redirect, strlen(redirect));
        }
    } else if (strstr((char *)req, "favicon.ico")) {
        respond("404 Not Found", "text/plain", "", 0);
    } else if (str_starts_with((char *)req, "GET /status.json")) {
        build_json();
        respond("200 OK", "application/json", page, page_len);
    } else if (str_starts_with((char *)req, "GET /basic")) {
        build_html();
        respond("200 OK", "text/html; charset=utf-8", page, page_len);
    } else {
        send_dashboard(headers);
    }
}

static void poll_one(unsigned i) {
    uint8_t sn = server_socks[i];
    switch (getSn_SR(sn)) {
    case SOCK_CLOSED:
        conn_active[i] = false;
        if (wz_socket(sn, Sn_MR_TCP, HTTP_PORT, SF_IO_NONBLOCK) == sn) {
            net_tcp_prepare(sn);
            wz_listen(sn);
        }
        break;
    case SOCK_INIT:
        wz_listen(sn);
        break;
    case SOCK_ESTABLISHED:
        if (!conn_active[i]) {
            conn_active[i] = true;
            conn_deadline[i] = make_timeout_time_ms(REQUEST_TIMEOUT_MS);
        }
        if (getSn_RX_RSR(sn) > 0) {
            cur_sn = sn;
            handle_request();
            wz_disconnect(sn);
            conn_active[i] = false;
        } else if (absolute_time_diff_us(get_absolute_time(), conn_deadline[i]) <= 0) {
            wz_disconnect(sn);
            conn_active[i] = false;
        }
        break;
    case SOCK_CLOSE_WAIT:
        wz_disconnect(sn);
        conn_active[i] = false;
        break;
    default:
        // Closing states (FIN_WAIT, LAST_ACK, ...): if the client is gone the
        // W5500 retries for ~30 s, keeping this listener unavailable. Force
        // the socket closed after 2 s.
        if (is_nil_time(closing_since[i])) {
            closing_since[i] = make_timeout_time_ms(2000);
        } else if (time_reached(closing_since[i])) {
            wz_close(sn);
            closing_since[i] = nil_time;
        }
        return;
    }
    closing_since[i] = nil_time;
}

void http_server_poll(void) {
    for (unsigned i = 0; i < NSERVERS; i++) poll_one(i);
}
