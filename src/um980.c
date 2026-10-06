#include "um980.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "config.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/uart.h"
#include "idle.h"
#include "pico/time.h"
#include "rtcm.h"
#include "util.h"

// ---------------------------------------------------------------------------
// COM1 control port: RX interrupt into a ring buffer
// ---------------------------------------------------------------------------
#define CTRL_RING 4096
static uint8_t ctrl_ring[CTRL_RING];
static volatile uint32_t ctrl_head, ctrl_tail;

static void ctrl_rx_irq(void) {
    while (uart_is_readable(UM980_CTRL_UART)) {
        uint8_t c = (uint8_t)uart_getc(UM980_CTRL_UART);
        uint32_t next = (ctrl_head + 1) % CTRL_RING;
        if (next != ctrl_tail) {
            ctrl_ring[ctrl_head] = c;
            ctrl_head = next;
        }
    }
}

static int ctrl_getc(void) {
    if (ctrl_tail == ctrl_head) return -1;
    uint8_t c = ctrl_ring[ctrl_tail];
    ctrl_tail = (ctrl_tail + 1) % CTRL_RING;
    return c;
}

static void ctrl_clear(void) {
    ctrl_tail = ctrl_head;
}

// ---------------------------------------------------------------------------
// COM2 data port: DMA into a 16 KB ring that keeps filling even while the
// CPUs are busy with flash operations. The channel re-triggers itself at the
// end of each lap; the completion IRQ counts laps.
// ---------------------------------------------------------------------------
#define DATA_RING_BITS 14
#define DATA_RING (1u << DATA_RING_BITS)
static uint8_t data_ring[DATA_RING] __attribute__((aligned(DATA_RING)));
static int data_dma = -1;
static volatile uint32_t data_laps;
static uint64_t data_last_total;
static uint64_t data_read_pos;

static void data_dma_irq(void) {
    if (dma_irqn_get_channel_status(1, (uint)data_dma)) {
        dma_irqn_acknowledge_channel(1, (uint)data_dma);
        data_laps++;
    }
}

static void data_dma_start(void) {
    data_dma = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config((uint)data_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, DATA_RING_BITS);
    channel_config_set_dreq(&c, uart_get_dreq(UM980_DATA_UART, false));
    dma_channel_configure((uint)data_dma, &c, data_ring, &uart_get_hw(UM980_DATA_UART)->dr,
                          dma_encode_transfer_count_with_self_trigger(DATA_RING), false);
    dma_irqn_set_channel_enabled(1, (uint)data_dma, true);
    irq_set_exclusive_handler(DMA_IRQ_1, data_dma_irq);
    irq_set_enabled(DMA_IRQ_1, true);
    dma_channel_start((uint)data_dma);
}

static uint64_t data_total_written(void) {
    uint32_t l1, l2, pos;
    do {
        l1 = data_laps;
        pos = (uint32_t)(dma_hw->ch[data_dma].write_addr - (uintptr_t)data_ring) & (DATA_RING - 1);
        l2 = data_laps;
    } while (l1 != l2);
    uint64_t total = (uint64_t)l1 * DATA_RING + pos;
    // The lap IRQ may not have run yet when the write pointer has wrapped.
    while (total < data_last_total) total += DATA_RING;
    data_last_total = total;
    return total;
}

size_t um980_data_available(uint32_t *overruns) {
    uint64_t total = data_total_written();
    uint64_t avail = total - data_read_pos;
    if (avail > DATA_RING - 512) {
        // The writer lapped (or nearly lapped) us: what we'd read is torn.
        data_read_pos = total;
        if (overruns) (*overruns)++;
        return 0;
    }
    return (size_t)avail;
}

size_t um980_data_peek(uint8_t *dst, size_t max) {
    size_t avail = (size_t)(data_last_total - data_read_pos);
    size_t n = avail < max ? avail : max;
    uint32_t start = (uint32_t)(data_read_pos & (DATA_RING - 1));
    size_t first = DATA_RING - start;
    if (first > n) first = n;
    memcpy(dst, data_ring + start, first);
    memcpy(dst + first, data_ring, n - first);
    return n;
}

void um980_data_consume(size_t n) {
    data_read_pos += n;
}

uint32_t um980_data_rx_total(void) {
    uint32_t l1, l2, pos;
    do {
        l1 = data_laps;
        pos = (uint32_t)(dma_hw->ch[data_dma].write_addr - (uintptr_t)data_ring) & (DATA_RING - 1);
        l2 = data_laps;
    } while (l1 != l2);
    return l1 * DATA_RING + pos;
}

void um980_data_flush(void) {
    data_read_pos = data_total_written();
}

void um980_uart_init(void) {
    uart_init(UM980_CTRL_UART, UM980_BAUDRATE);
    gpio_set_function(UM980_CTRL_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UM980_CTRL_RX_PIN, GPIO_FUNC_UART);
    uart_set_format(UM980_CTRL_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(UM980_CTRL_UART, true);
    irq_set_exclusive_handler(UART0_IRQ, ctrl_rx_irq);
    irq_set_enabled(UART0_IRQ, true);
    uart_set_irq_enables(UM980_CTRL_UART, true, false);
    printf("UART0 initialized at %d baud (Control - COM1)\n", UM980_BAUDRATE);

    uart_init(UM980_DATA_UART, UM980_BAUDRATE);
    gpio_set_function(UM980_DATA_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UM980_DATA_RX_PIN, GPIO_FUNC_UART);
    uart_set_format(UM980_DATA_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(UM980_DATA_UART, true);
    data_dma_start();
    printf("UART1 initialized at %d baud (Data - COM2, DMA ring %u bytes)\n", UM980_BAUDRATE, DATA_RING);
}

// ---------------------------------------------------------------------------
// Command / query
// ---------------------------------------------------------------------------
#define RESP_MAX 8192  // SATSINFOA with a full sky is 4+ KB
static char resp[RESP_MAX + 1];

static void ctrl_write(const char *s) {
    uart_write_blocking(UM980_CTRL_UART, (const uint8_t *)s, strlen(s));
}

// Collect the response: returns once 500 ms pass without new data after the
// first byte, or after timeout_ms. Returns NULL if nothing arrived.
static const char *read_response(uint32_t timeout_ms, size_t max) {
    size_t len = 0;
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    absolute_time_t quiet_until = nil_time;
    while (absolute_time_diff_us(get_absolute_time(), deadline) > 0 && len < max) {
        int c;
        bool got = false;
        while (len < max && (c = ctrl_getc()) >= 0) {
            resp[len++] = (char)c;
            got = true;
        }
        if (got) quiet_until = make_timeout_time_ms(500);
        if (len && absolute_time_diff_us(get_absolute_time(), quiet_until) <= 0) break;
        idle_sleep_ms(10);
    }
    resp[len] = '\0';
    // NULs from line noise would truncate string handling.
    for (size_t i = 0; i < len; i++)
        if (!resp[i]) resp[i] = ' ';
    return len ? resp : NULL;
}

// Query: plain command, no '$' or checksum (VERSIONA, CONFIG, MODE, ...).
static const char *send_query(const char *cmd, uint32_t timeout_ms) {
    ctrl_clear();
    ctrl_write(cmd);
    ctrl_write("\r\n");
    printf("Sent: %s\n", cmd);
    const char *r = read_response(timeout_ms, RESP_MAX);
    if (!r) printf("No response received\n");
    else {
        int lines = 0;
        for (const char *p = r; *p; p++) lines += *p == '\n';
        printf("Response: %u bytes, %d lines\n", (unsigned)strlen(r), lines);
    }
    return r;
}

// Configuration command: $<cmd>*<XOR checksum>.
static const char *send_command(const char *cmd, uint32_t timeout_ms) {
    uint8_t cs = 0;
    for (const char *p = cmd; *p; p++) cs ^= (uint8_t)*p;
    char line[160];
    snprintf(line, sizeof(line), "$%s*%02X", cmd, cs);
    ctrl_clear();
    ctrl_write(line);
    ctrl_write("\r\n");
    printf("Sent: %s\n", line);
    const char *r = read_response(timeout_ms, RESP_MAX);
    if (!r) printf("No response received\n");
    else if (strstr(r, "OK")) printf("Response: OK\n");
    else printf("Response: %u bytes\n", (unsigned)strlen(r));
    return r;
}

const char *um980_query(const char *cmd, uint32_t timeout_ms) {
    return send_query(cmd, timeout_ms);
}

const char *um980_query_log(const char *cmd, const char *tag, uint32_t timeout_ms, uint64_t *tag_time_us) {
    // A "once" log is printed at the receiver's next epoch, after the
    // command acknowledgement: read until the tagged line is complete.
    ctrl_clear();
    ctrl_write(cmd);
    ctrl_write("\r\n");
    size_t len = 0, tag_len = strlen(tag);
    const char *found = NULL;
    *tag_time_us = 0;
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!time_reached(deadline) && len < RESP_MAX) {
        int c = ctrl_getc();
        if (c < 0) {
            idle_poll();
            sleep_us(200);
            continue;
        }
        resp[len++] = (char)(c ? c : ' ');
        resp[len] = '\0';
        if (!found && len >= tag_len && memcmp(resp + len - tag_len, tag, tag_len) == 0) {
            found = resp + len - tag_len;
            *tag_time_us = time_us_64() - (uint64_t)tag_len * 87;  // ~87 us/char at 115200
        }
        if (found && c == '\n') return resp;
    }
    resp[len] = '\0';
    return len ? resp : NULL;  // something answered, even if not the log line
}

void um980_ctrl_set_baud(uint32_t baud) {
    uart_tx_wait_blocking(UM980_CTRL_UART);
    uart_set_baudrate(UM980_CTRL_UART, baud);
}

void um980_ctrl_write(const void *data, size_t len) {
    uart_write_blocking(UM980_CTRL_UART, data, len);
}

void um980_ctrl_discard_input(void) {
    ctrl_clear();
}

int um980_ctrl_read_byte(uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    for (;;) {
        int c = ctrl_getc();
        if (c >= 0) return c;
        if (time_reached(deadline)) return -1;
        idle_poll();
    }
}

bool um980_ctrl_wait_string(const char *needle, uint32_t timeout_ms) {
    char win[64];
    size_t n = strlen(needle), have = 0;
    if (n >= sizeof(win)) return false;
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!time_reached(deadline)) {
        int c = um980_ctrl_read_byte(50);
        if (c < 0) continue;
        if (have == n) {
            memmove(win, win + 1, n - 1);
            have--;
        }
        win[have++] = (char)c;
        if (have == n && memcmp(win, needle, n) == 0) return true;
    }
    return false;
}

int um980_ctrl_wait_byte(const uint8_t *accept, size_t n, uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!time_reached(deadline)) {
        int c = um980_ctrl_read_byte(50);
        if (c < 0) continue;
        for (size_t i = 0; i < n; i++)
            if (c == accept[i]) return c;
    }
    return -1;
}

// Iterate lines of a response in place (modifies the buffer).
static char *next_line(char **cursor) {
    char *s = *cursor;
    if (!s || !*s) return NULL;
    char *nl = strchr(s, '\n');
    if (nl) {
        *nl = '\0';
        *cursor = nl + 1;
    } else {
        *cursor = s + strlen(s);
    }
    size_t n = strlen(s);
    if (n && s[n - 1] == '\r') s[n - 1] = '\0';
    return s;
}

static void strip_quotes_copy(char *dst, size_t size, const char *src, size_t n) {
    char tmp[64];
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < sizeof(tmp); i++)
        if (src[i] != '"') tmp[o++] = src[i];
    tmp[o] = '\0';
    str_copy(dst, size, str_trim(tmp));
}

static bool get_receiver_model(char *model, size_t msize, char *fw, size_t fsize) {
    printf("\n=== Getting Receiver Model ===\n");
    const char *r = send_query("VERSIONA", 3000);
    if (!r || !strstr(r, "#VERSIONA")) return false;
    char *cur = resp, *line;
    while ((line = next_line(&cur))) {
        if (!strstr(line, "#VERSIONA")) continue;
        // #VERSIONA,...;"UM980","R4.10Build11833",...
        char *semi = strrchr(line, ';');
        char *fields = semi ? semi + 1 : line;
        char *comma = strchr(fields, ',');
        if (!comma) continue;
        char *comma2 = strchr(comma + 1, ',');
        size_t fw_len = comma2 ? (size_t)(comma2 - comma - 1) : strlen(comma + 1);
        strip_quotes_copy(model, msize, fields, (size_t)(comma - fields));
        strip_quotes_copy(fw, fsize, comma + 1, fw_len);
        printf("Model: %s, Firmware: %s\n", model, fw);
        return model[0] != '\0';
    }
    return false;
}

typedef struct {
    int signal_group;   // -1 unknown
    int sbas;           // -1 unknown, 0 disabled, 1 enabled
    char mode[64];
    int pps;  // -1 unknown, 0 disabled, 1 enabled
    bool rtcm_on_com2[16];
} um980_state_t;

static void get_current_config(um980_state_t *st) {
    printf("\n=== Reading Current Configuration ===\n");
    memset(st, 0, sizeof(*st));
    st->signal_group = -1;
    st->sbas = -1;
    st->pps = -1;

    char *cur, *line;
    if (send_query("CONFIG", 5000)) {
        cur = resp;
        while ((line = next_line(&cur))) {
            if (strstr(line, "SIGNALGROUP") && strstr(line, "CONFIG")) {
                // $CONFIG,SIGNALGROUP,CONFIG SIGNALGROUP 2*xx: the value is
                // the token after SIGNALGROUP in the third field.
                char *f = strchr(line, ',');
                f = f ? strchr(f + 1, ',') : NULL;
                if (!f) continue;
                char *star = strchr(++f, '*');
                if (star) *star = '\0';
                char *save, *tok = strtok_r(f, " \t", &save);
                while (tok && strcmp(tok, "SIGNALGROUP") != 0) tok = strtok_r(NULL, " \t", &save);
                tok = tok ? strtok_r(NULL, " \t", &save) : NULL;
                if (tok) {
                    st->signal_group = atoi(tok);
                    printf("✓ Signal Group: %d\n", st->signal_group);
                }
            } else if (strstr(line, "SBAS") && strstr(line, "CONFIG")) {
                if (str_icase_find(line, "ENABLE") && !str_icase_find(line, "DISABLE")) {
                    st->sbas = 1;
                    printf("✓ SBAS: Enabled\n");
                } else if (str_icase_find(line, "DISABLE")) {
                    st->sbas = 0;
                    printf("✓ SBAS: Disabled\n");
                }
            } else if (strstr(line, "CONFIG PPS")) {
                st->pps = strstr(line, "PPS ENABLE") ? 1 : 0;
                printf("✓ PPS: %s\n", st->pps ? "Enabled" : "Disabled");
            } else if (str_starts_with(line, "$CONFIG,COM")) {
                printf("✓ %s\n", line + 8);
            }
        }
    }

    if (send_query("MODE", 5000)) {
        cur = resp;
        while ((line = next_line(&cur))) {
            if (!str_starts_with(line, "#MODE")) continue;
            char *semi = strchr(line, ';');
            if (!semi) continue;
            char *star = strchr(semi + 1, '*');
            if (star) *star = '\0';
            char *m = str_trim(semi + 1);
            size_t n = strlen(m);
            while (n && m[n - 1] == ',') m[--n] = '\0';
            str_copy(st->mode, sizeof(st->mode), m);
            printf("✓ Mode: %s\n", st->mode);
        }
    }

    if (send_query("UNILOGLIST", 5000)) {
        printf("\n=== Active RTCM Messages ===\n");
        int active = 0;
        cur = resp;
        while ((line = next_line(&cur))) {
            if (!strstr(line, "RTCM") || !strstr(line, "COM")) continue;
            // "< RTCM1005 COM2 30"
            char lt[4], type[24], port[12], rate[12];
            if (sscanf(line, " %3s %23s %11s %11s", lt, type, port, rate) != 4 || strcmp(lt, "<") != 0) continue;
            printf("  %s on %s @ %ss\n", type, port, rate);
            active++;
            if (strcmp(port, "COM2") != 0) continue;
            for (size_t i = 0; i < rtcm_message_count && i < 16; i++)
                if (strcmp(type, rtcm_messages[i].name) == 0) st->rtcm_on_com2[i] = true;
        }
        if (!active) printf("  No RTCM messages active\n");
    }
}

static bool config_needs_update(const um980_state_t *st) {
    bool needs = false;
    bool want_sbas = config.sbas_enabled;
    if (st->signal_group != config.signal_group) {
        printf("\n✗ Signal group: %d (want %d)\n", st->signal_group, config.signal_group);
        needs = true;
    }
    if ((st->sbas == 1) != want_sbas) {
        printf("✗ SBAS %s (should be %s)\n", st->sbas == 1 ? "enabled" : "disabled",
               want_sbas ? "enabled" : "disabled");
        needs = true;
    }
    if (st->pps == 0) {
        printf("✗ PPS disabled (should be enabled)\n");
        needs = true;
    }
    if (st->mode[0] && !str_icase_find(st->mode, "BASE")) {
        printf("✗ Not in base mode: %s\n", st->mode);
        needs = true;
    }
    bool missing_any = false;
    for (size_t i = 0; i < rtcm_message_count && i < 16; i++) {
        if (!st->rtcm_on_com2[i]) {
            if (!missing_any) printf("\n✗ Missing RTCM on COM2:");
            printf(" %s", rtcm_messages[i].name);
            missing_any = true;
        }
    }
    if (missing_any) {
        printf("\n");
        needs = true;
    }
    if (!needs) printf("\n✓ Configuration is correct\n");
    return needs;
}

static void configure(const um980_state_t *st) {
    char cmd[128];
    printf("==================================================\n");
    printf("UM980 CONFIGURATION\n");
    printf("==================================================\n");
    printf("\n=== Configuring Base Station ===\n");

    // SIGNALGROUP reboots the receiver: only send it when it must change.
    if (st->signal_group != config.signal_group) {
        printf("Setting signal group %d...\n", config.signal_group);
        snprintf(cmd, sizeof(cmd), "CONFIG SIGNALGROUP %d", config.signal_group);
        send_command(cmd, 2000);
        printf("Waiting for reboot (10s)...\n");
        idle_sleep_ms(10000);
    } else {
        printf("Signal group already %d, skipping reboot\n", config.signal_group);
    }

    // PPS: rising edge at the start of each GPS second, 500 ms wide.
    printf("Enabling PPS...\n");
    send_command("CONFIG PPS ENABLE GPS POSITIVE 500000 1000 0 0", 2000);
    idle_sleep_ms(200);

    printf("%s SBAS...\n", config.sbas_enabled ? "Enabling" : "Disabling");
    send_command(config.sbas_enabled ? "CONFIG SBAS ENABLE AUTO" : "CONFIG SBAS DISABLE", 2000);
    idle_sleep_ms(500);

    if (strcmp(config.base_mode, "fixed") == 0) {
        if (config.base_lat < -90 || config.base_lat > 90 || config.base_lon < -180 || config.base_lon > 180 ||
            config.base_alt < -30000 || config.base_alt > 30000) {
            printf("✗ Fixed base coordinates out of range, falling back to self-survey\n");
            snprintf(cmd, sizeof(cmd), "MODE BASE 1 TIME %d %g", config.base_duration, config.base_pdop);
        } else {
            snprintf(cmd, sizeof(cmd), "MODE BASE %.9f %.9f %.4f", config.base_lat, config.base_lon, config.base_alt);
        }
    } else {
        snprintf(cmd, sizeof(cmd), "MODE BASE 1 TIME %d %g", config.base_duration, config.base_pdop);
    }
    printf("Setting base mode: %s\n", cmd);
    send_command(cmd, 2000);
    idle_sleep_ms(500);

    printf("\n=== Configuring RTCM on COM2 ===\n");
    for (size_t i = 0; i < rtcm_message_count; i++) {
        int interval = rtcm_messages[i].interval_s ? rtcm_messages[i].interval_s : config.rtcm_interval;
        if (interval < 1) interval = 1;
        snprintf(cmd, sizeof(cmd), "%s COM2 %d", rtcm_messages[i].name, interval);
        printf("  %s @ %ds\n", rtcm_messages[i].name, interval);
        send_command(cmd, 2000);
        idle_sleep_ms(200);
    }

    printf("\n=== Saving Configuration ===\n");
    const char *r = send_command("SAVECONFIG", 3000);
    if (r && strstr(r, "OK")) printf("✓ Configuration saved to NVM\n");
    else printf("✗ SAVECONFIG may have failed\n");
    idle_sleep_ms(1000);

    printf("\n==================================================\n");
    printf("CONFIGURATION COMPLETE\n");
    printf("==================================================\n");
}

bool um980_start(char *model, size_t model_size, char *firmware, size_t fw_size) {
    printf("\n=== Starting UM980 Sensor ===\n");
    bool found = false;
    for (int retry = 0; retry < 5 && !found; retry++) {
        found = get_receiver_model(model, model_size, firmware, fw_size);
        if (!found) {
            printf("⚠ Retry %d/5: Failed to detect UM980\n", retry + 1);
            idle_sleep_ms(1000);
        }
    }
    if (!found) {
        printf("✗ Failed to detect UM980 after retries\n");
        return false;
    }
    printf("✓ UM980 detected: %s, FW: %s\n", model, firmware);

    printf("\n=== Checking UM980 Configuration ===\n");
    static um980_state_t st;
    get_current_config(&st);
    if (config_needs_update(&st)) {
        printf("\n⚠ UM980 needs configuration update, configuring automatically...\n");
        configure(&st);
        printf("✓ UM980 configured\n");
    } else {
        printf("✓ UM980 already configured correctly\n");
    }
    return true;
}

bool um980_get_agc(agc_values_t *out) {
    printf("\n=== Getting AGC Values ===\n");
    for (int attempt = 1; attempt <= 9; attempt++) {
        const char *r = send_query("AGCA", 3000);
        if (!r) {
            printf("Attempt %d/9: No response\n", attempt);
            idle_sleep_ms(500);
            continue;
        }
        if (strstr(r, "$command") && !strstr(r, "#AGCA")) {
            printf("Attempt %d/9: Got command ACK only, retrying...\n", attempt);
            idle_sleep_ms(500);
            continue;
        }
        // #AGCA,98,GPS,UNKNOWN,1,5544000,0,0,18,3;95,74,83,-1,...*f831d559
        char *cur = resp, *line;
        while ((line = next_line(&cur))) {
            if (!strstr(line, "#AGCA")) continue;
            char *semi = strrchr(line, ';');
            if (!semi) continue;
            int v[3];
            if (sscanf(semi + 1, "%d,%d,%d", &v[0], &v[1], &v[2]) == 3) {
                out->l1 = v[0];
                out->l2 = v[1];
                out->l5 = v[2];
                printf("L1: %d, L2: %d, L5: %d\n", v[0], v[1], v[2]);
                return true;
            }
            printf("Attempt %d/9: ERROR parsing AGCA response\n", attempt);
        }
        idle_sleep_ms(500);
    }
    printf("ERROR: Failed to get valid AGCA response after 9 attempts\n");
    return false;
}

agc_state_t um980_agc_state(int value) {
    if (value == -1) return AGC_UNKNOWN;
    return value >= 0 && value < 10 ? AGC_GOOD : AGC_BAD;
}
