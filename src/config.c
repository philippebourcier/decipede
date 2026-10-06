#include "config.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "build_config.h"
#include "flash_store.h"
#include "http_client.h"
#include "idle.h"
#include "json_util.h"
#include "util.h"

app_config_t config;

typedef enum { F_BOOL, F_INT, F_DOUBLE, F_STR } field_type_t;

typedef struct {
    const char *key;
    field_type_t type;
    size_t offset;
    size_t size;      // for strings
    bool secret;      // not printed
    bool remote_ok;   // may be set by the remote config
} field_t;

#define FB(k, rem)        {#k, F_BOOL, offsetof(app_config_t, k), 0, false, rem}
#define FI(k, rem)        {#k, F_INT, offsetof(app_config_t, k), 0, false, rem}
#define FD(k, rem)        {#k, F_DOUBLE, offsetof(app_config_t, k), 0, false, rem}
#define FS(k, sec, rem)   {#k, F_STR, offsetof(app_config_t, k), sizeof(((app_config_t *)0)->k), sec, rem}

static const field_t fields[] = {
    FB(dhcp, true),
    FS(ip, false, true),
    FS(subnet, false, true),
    FS(gateway, false, true),
    FS(dns, false, true),
    FS(base_mode, false, true),
    FI(base_duration, true),
    FD(base_pdop, true),
    FD(base_lat, true),
    FD(base_lon, true),
    FD(base_alt, true),
    FI(signal_group, true),
    FB(sbas_enabled, true),
    FI(rtcm_interval, true),
    FS(ntrip_server, false, true),
    FI(ntrip_port, true),
    FS(ntrip_mountpoint, false, true),
    FS(ntrip_user, false, true),
    FS(ntrip_password, true, true),
    // The config URL is provisioning-only: a bad remote value would cut the
    // device off from its config server for good.
    FS(config_url, false, false),
    FS(telemetry_url, false, true),
    FS(firmware_url, false, true),
    FI(ota_interval_h, true),
    FI(tcp_mss, true),
    FB(gnss_watchdog, true),
    FS(maint_window, false, true),
    FB(ntp_server, true),
    FI(download_jitter_s, true),
};
#define NFIELDS (sizeof(fields) / sizeof(fields[0]))

void config_set_defaults(void) {
    memset(&config, 0, sizeof(config));
    config.dhcp = true;
    str_copy(config.base_mode, sizeof(config.base_mode), "time");
    config.base_duration = 60;
    config.base_pdop = 1;
    config.signal_group = 2;
    config.sbas_enabled = true;
    config.rtcm_interval = 1;
    // Caster, telemetry, OTA and UM980 firmware URLs: from config.json only.
    config.ntrip_port = 2101;  // NTRIP's standard port, if config.json gives none
    str_copy(config.config_url, sizeof(config.config_url), DEFAULT_CONFIG_URL);
    config.ota_interval_h = 6;
    config.gnss_watchdog = true;
    config.download_jitter_s = 3600;
}

static void *field_ptr(const field_t *f) {
    return (char *)&config + f->offset;
}

static void field_to_text(const field_t *f, char *out, size_t out_size) {
    void *p = field_ptr(f);
    switch (f->type) {
    case F_BOOL: snprintf(out, out_size, "%s", *(bool *)p ? "true" : "false"); break;
    case F_INT: snprintf(out, out_size, "%d", *(int *)p); break;
    case F_DOUBLE: snprintf(out, out_size, "%.10g", *(double *)p); break;
    case F_STR: snprintf(out, out_size, "%s", (char *)p); break;
    }
}

// Set field f from JSON token t. Returns 1 if changed, 0 if unchanged,
// -1 if the value has the wrong type.
static int field_set(const field_t *f, const json_doc_t *doc, int t) {
    void *p = field_ptr(f);
    switch (f->type) {
    case F_BOOL: {
        bool v;
        if (!json_tok_bool(doc, t, &v)) return -1;
        if (*(bool *)p == v) return 0;
        *(bool *)p = v;
        return 1;
    }
    case F_INT: {
        double v;
        if (!json_tok_double(doc, t, &v)) return -1;
        int iv = (int)lround(v);
        if (*(int *)p == iv) return 0;
        *(int *)p = iv;
        return 1;
    }
    case F_DOUBLE: {
        double v;
        if (!json_tok_double(doc, t, &v)) return -1;
        if (*(double *)p == v) return 0;
        *(double *)p = v;
        return 1;
    }
    case F_STR: {
        char tmp[CFG_URL];
        size_t raw_len = (size_t)(doc->tok[t].end - doc->tok[t].start);
        if (!json_is_null(doc, t) && raw_len >= sizeof(tmp)) {
            printf("  %s: value too long, ignored\n", f->key);
            return -1;
        }
        if (json_is_null(doc, t)) tmp[0] = '\0';
        else if (json_is_string(doc, t) || doc->tok[t].type == JSMN_PRIMITIVE) json_tok_str(doc, t, tmp, sizeof(tmp));
        else return -1;
        if (strlen(tmp) >= f->size) {
            printf("  %s: value too long (max %u), ignored\n", f->key, (unsigned)(f->size - 1));
            return -1;
        }
        if (strcmp((char *)p, tmp) == 0) return 0;
        str_copy((char *)p, f->size, tmp);
        return 1;
    }
    }
    return -1;
}

static const field_t *find_field(const json_doc_t *doc, int key_tok) {
    for (size_t i = 0; i < NFIELDS; i++) {
        if (json_tok_eq(doc, key_tok, fields[i].key)) return &fields[i];
    }
    return NULL;
}

static int serialize(char *out, size_t out_size) {
    size_t pos = 0;
    out[pos++] = '{';
    for (size_t i = 0; i < NFIELDS; i++) {
        const field_t *f = &fields[i];
        if (pos + 4 >= out_size) return -1;
        if (i) out[pos++] = ',';
        pos = json_append_string(out, pos, out_size, f->key);
        if (pos + 2 >= out_size) return -1;
        out[pos++] = ':';
        if (f->type == F_STR) {
            const char *s = field_ptr(f);
            if (!*s && f->offset != offsetof(app_config_t, base_mode)) {
                if (pos + 5 >= out_size) return -1;
                memcpy(out + pos, "null", 4);
                pos += 4;
            } else {
                pos = json_append_string(out, pos, out_size, s);
            }
        } else {
            char v[32];
            field_to_text(f, v, sizeof(v));
            size_t n = strlen(v);
            if (pos + n + 2 >= out_size) return -1;
            memcpy(out + pos, v, n);
            pos += n;
        }
        if (pos >= out_size) return -1;
    }
    if (pos + 2 >= out_size) return -1;
    out[pos++] = '}';
    out[pos] = '\0';
    return (int)pos;
}

bool config_load_local(void) {
    static char buf[BLOB_MAX + 1];
    int len = blob_load(BLOB_CONFIG, buf, BLOB_MAX);
    if (len < 0) {
        printf("No local config stored, using defaults\n");
        return false;
    }
    buf[len] = '\0';
    static json_doc_t doc;
    if (!json_parse(&doc, buf, (size_t)len)) {
        printf("ERROR loading local config: invalid JSON\n");
        return false;
    }
    int i = 0, k, v;
    while (json_next_member(&doc, &i, &k, &v)) {
        const field_t *f = find_field(&doc, k);
        if (f) field_set(f, &doc, v);
    }
    config.loaded = true;
    printf("✓ Local config loaded\n");
    return true;
}

bool config_save_local(void) {
    static char buf[BLOB_MAX];
    int len = serialize(buf, sizeof(buf));
    if (len < 0) {
        printf("ERROR saving config: too large\n");
        return false;
    }
    if (!blob_save(BLOB_CONFIG, buf, (size_t)len)) {
        printf("ERROR saving config: flash write failed\n");
        return false;
    }
    printf("✓ Config saved locally\n");
    return true;
}

bool config_apply_remote(const char *json, int len) {
    static json_doc_t doc;
    if (!json_parse(&doc, json, (size_t)len)) {
        printf("ERROR: remote config is not a JSON object\n");
        return false;
    }
    int members = 0, i = 0, k, v;
    bool changed = false;
    while (json_next_member(&doc, &i, &k, &v)) {
        members++;
        char key[32];
        json_tok_str(&doc, k, key, sizeof(key));
        const field_t *f = find_field(&doc, k);
        if (!f) {
            printf("  %s: unknown key, ignored\n", key);
            continue;
        }
        if (!f->remote_ok) {
            printf("  %s: not settable remotely, ignored\n", key);
            continue;
        }
        char old[CFG_URL];
        field_to_text(f, old, sizeof(old));
        int rc = field_set(f, &doc, v);
        if (rc < 0) {
            printf("  %s: invalid value, ignored\n", key);
        } else if (rc == 0) {
            printf("  %s: unchanged\n", key);
        } else {
            changed = true;
            char now[CFG_URL];
            field_to_text(f, now, sizeof(now));
            if (f->secret) printf("  %s: changed\n", key);
            else printf("  %s: %s -> %s\n", key, old, now);
        }
    }
    printf("Received config: %d settings\n", members);
    config.loaded = true;
    if (changed) {
        config_save_local();
        printf("✓ Configuration loaded and saved\n");
    } else {
        printf("✓ Configuration loaded, no changes (flash write skipped)\n");
    }
    return true;
}

#define CONFIG_TLS_ATTEMPTS 3
#define CONFIG_RETRY_DELAY_MS 2000

static const char *config_src = "local";

bool config_download(void) {
    static char body[BLOB_MAX];
    char url[CFG_URL + 32];

    printf("\n=== Downloading Configuration ===\n");
    printf("Hardware ID: %s\n", hardware_id());
    if (!config.config_url[0]) {
        printf("ERROR: no config_url configured\n");
        return false;
    }
    snprintf(url, sizeof(url), "%s?b=%s", config.config_url, hardware_id());

    // https:// is tried CONFIG_TLS_ATTEMPTS times. If none of them got an HTTP
    // response (TLS broken: certificate chain the firmware can't verify,
    // server misconfiguration, ...), the same URL is fetched over plain http://
    // so that the config, and through it the OTA URLs, can still reach a
    // device whose TLS trust store has gone stale. HTTPS is tried first again
    // on every download.
    bool tls = str_starts_with(url, "https://");
    int status = 0;
    int len = -1;
    for (int i = 0; i < (tls ? CONFIG_TLS_ATTEMPTS : 1); i++) {
        if (i) {
            printf("Retrying config download (%d/%d)\n", i + 1, CONFIG_TLS_ATTEMPTS);
            idle_sleep_ms(CONFIG_RETRY_DELAY_MS);
        }
        len = http_get_to_buffer(url, body, sizeof(body) - 1, &status, 10000);
        if (len >= 0 || status) break;  // an HTTP response: TLS itself works
    }
    const char *source = tls ? "https" : "http";
    if (len < 0 && !status && tls) {
        char plain[sizeof(url)];
        snprintf(plain, sizeof(plain), "http://%s", url + 8);
        printf("WARNING: config over HTTPS failed %d times, falling back to %s\n", CONFIG_TLS_ATTEMPTS, plain);
        len = http_get_to_buffer(plain, body, sizeof(body) - 1, &status, 10000);
        source = "http (HTTPS failed)";
    }
    if (len < 0) {
        printf("ERROR downloading config\n");
        return false;
    }
    if (status != 200) {
        printf("ERROR: HTTP %d\n", status);
        return false;
    }
    printf("✓ HTTP 200 OK\n");
    body[len] = '\0';
    if (!config_apply_remote(body, len)) return false;
    config_src = source;
    return true;
}

const char *config_source(void) {
    return config_src;
}

void config_print(void) {
    printf("\n=== Current Configuration ===\n");
    for (size_t i = 0; i < NFIELDS; i++) {
        char v[CFG_URL];
        if (fields[i].secret && *(char *)field_ptr(&fields[i])) str_copy(v, sizeof(v), "********");
        else field_to_text(&fields[i], v, sizeof(v));
        printf("  %s: %s\n", fields[i].key, v[0] ? v : "None");
    }
}
