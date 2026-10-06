#include "fw_manifest.h"

#include <stdio.h>
#include <string.h>

#include "app_status.h"
#include "config.h"
#include "http_client.h"
#include "pico/time.h"
#include "util.h"

#define CACHE_MS 60000
#define FETCH_TIMEOUT_MS 15000

static char body[2048];
static int body_len = -1;
static absolute_time_t fetched_at;
static json_doc_t top;

void fw_manifest_invalidate(void) {
    body_len = -1;
}

static fwm_status_t fetch(void) {
    if (body_len >= 0 && absolute_time_diff_us(fetched_at, get_absolute_time()) < CACHE_MS * 1000LL) return FWM_OK;
    body_len = -1;
    char url[CFG_URL + 128];
    snprintf(url, sizeof(url), "%s%cb=%s&fw=%s&um980=%s", config.firmware_url,
             strchr(config.firmware_url, '?') ? '&' : '?', hardware_id(), FW_VERSION_STRING,
             app_status.um980_firmware[0] ? app_status.um980_firmware : "unknown");
    printf("\n=== Firmware manifest: %s ===\n", config.firmware_url);
    int status = 0;
    int len = http_get_to_buffer(url, body, sizeof(body) - 1, &status, FETCH_TIMEOUT_MS);
    if (len < 0 || status != 200) {
        printf("Firmware manifest unavailable (HTTP %d)\n", status);
        return FWM_UNAVAILABLE;
    }
    body[len] = '\0';
    if (!json_parse(&top, body, (size_t)len)) {
        printf("Firmware manifest: invalid JSON\n");
        return FWM_INVALID;
    }
    body_len = len;
    fetched_at = get_absolute_time();
    return FWM_OK;
}

fwm_status_t fw_manifest_section(const char *name, json_doc_t *section) {
    if (!config.firmware_url[0]) return FWM_DISABLED;
    fwm_status_t st = fetch();
    if (st != FWM_OK) return st;
    int t = json_find(&top, name);
    if (t < 0 || json_is_null(&top, t)) return FWM_ABSENT;
    if (top.tok[t].type != JSMN_OBJECT) return FWM_INVALID;
    const char *start = body + top.tok[t].start;
    if (!json_parse(section, start, (size_t)(top.tok[t].end - top.tok[t].start))) return FWM_INVALID;
    return FWM_OK;
}
