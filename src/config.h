// Device configuration, port of config_manager.py.
//
// Same keys as the Python config.json (plus the former .env URLs and the
// OTA settings), persisted as JSON in the Config flash partition.
#pragma once

#include <stdbool.h>

#define CFG_STR_SHORT 16
#define CFG_STR       64
#define CFG_URL       192

typedef struct {
    // Network
    bool dhcp;
    char ip[CFG_STR_SHORT];
    char subnet[CFG_STR_SHORT];
    char gateway[CFG_STR_SHORT];
    char dns[CFG_STR_SHORT];
    // Base
    char base_mode[8];  // "time" or "fixed"
    int base_duration;
    double base_pdop;
    double base_lat;
    double base_lon;
    double base_alt;
    int signal_group;
    bool sbas_enabled;
    int rtcm_interval;
    // NTRIP
    char ntrip_server[CFG_STR];
    int ntrip_port;
    char ntrip_mountpoint[CFG_STR];
    char ntrip_user[CFG_STR];
    char ntrip_password[CFG_STR];
    // Cloud (was .env)
    char config_url[CFG_URL];
    char telemetry_url[CFG_URL];
    // OTA
    char firmware_url[CFG_URL];  // firmware manifest, both chips (fw_manifest.h)
    int ota_interval_h;
    // Network tuning
    int tcp_mss;  // 0 = automatic
    // GNSS receiver
    bool gnss_watchdog;
    char maint_window[16];      // "HH:MM-HH:MM" UTC, for UM980 upgrades
    // Services
    bool ntp_server;
    int download_jitter_s;      // random delay before scheduled downloads
    // Not persisted
    bool loaded;
} app_config_t;

extern app_config_t config;

// Defaults + provisioning URLs baked in at build time.
void config_set_defaults(void);
// Load from flash. Returns false (keeping defaults) if nothing stored.
bool config_load_local(void);
bool config_save_local(void);
// Merge a remote JSON config. Saves to flash only if something changed.
// Returns false if the JSON is invalid.
bool config_apply_remote(const char *json, int len);
// GET <config_url>?b=<hwid> and merge. Returns true on success.
bool config_download(void);
// Where the last successfully applied remote config came from: "local" (none
// yet), "https", "http", or "http (HTTPS failed)" for the plain-HTTP fallback.
const char *config_source(void);
void config_print(void);
