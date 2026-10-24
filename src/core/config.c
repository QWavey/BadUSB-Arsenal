/*
 * GhostLink config.txt parser.
 *
 * Not a general INI parser: deliberately strict so a typo surfaces loudly in
 * the boot log instead of being silently ignored. Parses in a single pass,
 * O(keys * schema), no allocations after startup.
 */

#include "config.h"
#include "../common/log.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>

ghost_cfg_t g_cfg;
volatile uint64_t g_cfg_gen = 0;

static int bool_of(const char *v) {
    if (!v) return 0;
    return (!strcasecmp(v, "TRUE") || !strcasecmp(v, "YES") ||
            !strcasecmp(v, "ON")   || !strcmp(v, "1"));
}

/* Copies src into dst. Returns 1 when src did not fit and was cut. */
static int cp(char *dst, size_t cap, const char *src) {
    if (!src) { dst[0] = 0; return 0; }
    size_t n = strlen(src);
    int truncated = (n >= cap);
    if (truncated) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
    return truncated;
}

void cfg_set_default(ghost_cfg_t *c) {
    memset(c, 0, sizeof(*c));

    cp(c->hostname, sizeof(c->hostname), "ghostlink");
    cp(c->device_name, sizeof(c->device_name), "WebBee-Ghost");
    cp(c->timezone, sizeof(c->timezone), "UTC");
    cp(c->locale, sizeof(c->locale), "en_US.UTF-8");
    c->fast_boot = 1;
    c->boot_led  = 1;

    c->usb = 1;
    cp(c->usb_mode, sizeof(c->usb_mode), "NCM");
    c->usb_vid = 0x1D6B;
    c->usb_pid = 0x0137;
    cp(c->usb_manufacturer, sizeof(c->usb_manufacturer), "GhostLink");
    cp(c->usb_product, sizeof(c->usb_product), "WebBee Passthrough");
    cp(c->usb_serial, sizeof(c->usb_serial), "AUTO");
    c->usb_hid = 1;

    c->passthrough_lan = 1;
    cp(c->netmode, sizeof(c->netmode), "BRIDGE");
    c->assign_ip = 1;
    cp(c->dhcp_range, sizeof(c->dhcp_range), "172.16.24.10,172.16.24.60");
    cp(c->gateway_ip, sizeof(c->gateway_ip), "172.16.24.1");
    cp(c->dns_upstream, sizeof(c->dns_upstream), "1.1.1.1,9.9.9.9");
    cp(c->mac_clone_from, sizeof(c->mac_clone_from), "eth0");

    c->log_to_sd = 1;
    cp(c->record_format, sizeof(c->record_format), "PCAPNG");
    c->record_rotate_mb  = 64;
    c->record_ring_files = 32;
    c->verdict_stream    = 1;

    c->webui = 1;
    c->webui_port = 80;
    cp(c->webui_bind, sizeof(c->webui_bind), "0.0.0.0");
    cp(c->webui_user, sizeof(c->webui_user), "root");
    /* No default password: httpd stays off until WEBUI_PASS or API_TOKEN is set. */
    cp(c->webui_pass, sizeof(c->webui_pass), "");
    cp(c->webui_lang, sizeof(c->webui_lang), "en");

    c->deadnet_ipv4 = 1;
    c->deadnet_ipv6 = 1;
    c->deadnet_interval_ms = 800;

    cp(c->redirect_proto, sizeof(c->redirect_proto), "AUTO");
    c->redirect_preserve_path = 1;

    cp(c->dns_spoof_file, sizeof(c->dns_spoof_file), "/Spoofhosts/default.hosts");

    cp(c->honeypot_ports,  sizeof(c->honeypot_ports),  "22,80,3389,445");
    cp(c->honeypot_log,    sizeof(c->honeypot_log),    "/Honeypot/sessions.jsonl");
    cp(c->honeypot_banner, sizeof(c->honeypot_banner), "OpenSSH_8.9p1");

    c->hid_enabled = 1;
    cp(c->hid_layout, sizeof(c->hid_layout), "us");
    cp(c->hid_trigger, sizeof(c->hid_trigger), "BUTTON");
    c->hid_delay_ms = 1500;

    c->payloads = 1;
    c->payload_tap_window_ms = 800;

    c->ssh = 1;
    c->ssh_port = 22;
    c->ssh_password_auth = 0;
    c->ssh_root = 0;

    c->mdns = 1;
    cp(c->mdns_name, sizeof(c->mdns_name), "ghostlink.local");

    c->loot = 1;
    cp(c->loot_priority, sizeof(c->loot_priority), "HIGH");

    cp(c->vpn_type, sizeof(c->vpn_type), "WIREGUARD");

    cp(c->wifi_mode, sizeof(c->wifi_mode), "STA");
    cp(c->wifi_ap_ssid, sizeof(c->wifi_ap_ssid), "GhostLink");
    cp(c->wifi_ap_pass, sizeof(c->wifi_ap_pass), "");

    c->cron = 1;
    cp(c->cron_file, sizeof(c->cron_file), "/Scripts/cron.txt");

    c->led = 1;
    cp(c->led_idle,      sizeof(c->led_idle),      "G:solid");
    cp(c->led_recording, sizeof(c->led_recording), "Y:single");
    cp(c->led_attack,    sizeof(c->led_attack),    "R:fast");
    cp(c->led_webui,     sizeof(c->led_webui),     "C:slow");

    c->alerts = 1;
    c->alert_on_new_host = 1;
    c->alert_on_rule_hit = 1;
    c->alert_on_honeypot = 1;

    c->flows = 1;
    c->flow_timeout_s = 180;
    c->flow_max = 4096;

    c->rate_limit_kbps = 256;

    c->ntp = 1;
    cp(c->ntp_servers, sizeof(c->ntp_servers), "pool.ntp.org,time.cloudflare.com");

    c->watchdog = 1;
    c->watchdog_timeout_s = 30;

    c->panic_hold_s = 10;
    c->panic_scrub  = 0;

    cp(c->ota_pubkey, sizeof(c->ota_pubkey), "/Firmware/ota.pub.pem");

    cp(c->log_level, sizeof(c->log_level), "INFO");
    c->log_to_serial = 1;
    cp(c->log_to_sd_file, sizeof(c->log_to_sd_file), "/Logs/ghostlink.log");

    cp(c->modules_dir, sizeof(c->modules_dir), "/Modules");

    c->factory_reset_taps = 5;
}

/* Trim trailing CR/LF/space in-place. */
static void rtrim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' ||
                 s[n-1] == ' '  || s[n-1] == '\t')) {
        s[--n] = 0;
    }
}

/* Skip leading whitespace. */
static char *ltrim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Values that reach shell command strings in bridge.c and main.c are checked
 * here, at parse time, so those files need no change. A rejected value leaves
 * the field unchanged and logs a warning. */
#define CS_IPV4  "0123456789."
#define CS_RANGE "0123456789.,"
#define CS_IFACE "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-"
#define CS_PATH  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._/-"

static int charset_ok(const char *v, const char *allowed, size_t maxlen) {
    size_t n = strlen(v);
    if (n > maxlen) return 0;
    for (size_t i = 0; i < n; i++)
        if (!strchr(allowed, v[i])) return 0;
    return 1;
}

/* Payload paths are joined onto /mnt/sd and handed to 'sh'. Empty is allowed. */
static int payload_path_ok(const char *v, size_t maxlen) {
    if (!v[0]) return 1;
    if (v[0] != '/' || strstr(v, "..")) return 0;
    return charset_ok(v, CS_PATH, maxlen);
}

int cfg_set_kv(ghost_cfg_t *c, const char *key, const char *val) {
    if (!key) return -1;
    /* Giant switch: one line per schema key. Boring on purpose. */
#define S(k, field)  if (!strcasecmp(key, k)) { \
    if (cp(c->field, sizeof(c->field), val)) \
        LOGW("cfg", "%s cut to %zu chars (value too long)", k, sizeof(c->field) - 1); \
    return 0; }
#define B(k, field)  if (!strcasecmp(key, k)) { c->field = bool_of(val); return 0; }
#define I(k, field)  if (!strcasecmp(key, k)) { c->field = (int)strtol(val, NULL, 0); return 0; }
#define H(k, field)  if (!strcasecmp(key, k)) { c->field = (uint16_t)strtoul(val, NULL, 0); return 0; }
/* V: copy only if the value uses an allowed charset; returns -2 when rejected. */
#define V(k, field, allowed) if (!strcasecmp(key, k)) { \
    if (!charset_ok(val, allowed, sizeof(c->field) - 1)) { \
        LOGW("cfg", "%s rejected (bad characters or too long); field unchanged", k); \
        return -2; } \
    cp(c->field, sizeof(c->field), val); return 0; }
/* P: payload path, see payload_path_ok(). */
#define P(k, field) if (!strcasecmp(key, k)) { \
    if (!payload_path_ok(val, sizeof(c->field) - 1)) { \
        LOGW("cfg", "%s rejected (must be /path with [A-Za-z0-9._/-], no ..); field unchanged", k); \
        return -2; } \
    cp(c->field, sizeof(c->field), val); return 0; }

    S("HOSTNAME", hostname);
    S("DEVICE_NAME", device_name);
    S("TIMEZONE", timezone);
    S("LOCALE", locale);
    B("FAST_BOOT", fast_boot);
    B("BOOT_LED", boot_led);

    B("USB", usb);
    S("USB_MODE", usb_mode);
    H("USB_VID", usb_vid);
    H("USB_PID", usb_pid);
    S("USB_MANUFACTURER", usb_manufacturer);
    S("USB_PRODUCT", usb_product);
    S("USB_SERIAL", usb_serial);
    B("USB_HID", usb_hid);
    B("USB_STORAGE", usb_storage);

    B("PASSTHROUGH_LAN", passthrough_lan);
    S("NETMODE", netmode);
    B("ASSIGN_IP", assign_ip);
    V("DHCP_RANGE", dhcp_range, CS_RANGE);
    V("GATEWAY_IP", gateway_ip, CS_IPV4);
    S("DNS_UPSTREAM", dns_upstream);
    B("MAC_CLONE", mac_clone);
    V("MAC_CLONE_FROM", mac_clone_from, CS_IFACE);
    V("STATIC_IP", static_ip, CS_IPV4);
    V("STATIC_NETMASK", static_netmask, CS_IPV4);
    V("STATIC_GATEWAY", static_gateway, CS_IPV4);

    B("LOG_TO_SD", log_to_sd);
    S("RECORD_FORMAT", record_format);
    I("RECORD_ROTATE_MB", record_rotate_mb);
    I("RECORD_RING_FILES", record_ring_files);
    S("RECORD_FILTER", record_filter);
    B("VERDICT_STREAM", verdict_stream);
    B("VERDICT_INCLUDE_PAYLOAD", verdict_include_payload);

    B("WEBUI", webui);
    I("WEBUI_PORT", webui_port);
    S("WEBUI_BIND", webui_bind);
    S("WEBUI_USER", webui_user);
    S("WEBUI_PASS", webui_pass);
    S("WEBUI_LANG", webui_lang);

    B("DEADNET", deadnet);
    B("DEADNET_IPV4", deadnet_ipv4);
    B("DEADNET_IPV6", deadnet_ipv6);
    I("DEADNET_INTERVAL_MS", deadnet_interval_ms);
    S("DEADNET_SPARE", deadnet_spare);
    B("DEADNET_SELECTIVE", deadnet_selective);
    S("DEADNET_TARGETS", deadnet_targets);

    B("REDIRECT", redirect);
    S("REDIRECT_FROM", redirect_from);
    S("REDIRECT_TO", redirect_to);
    S("REDIRECT_PROTO", redirect_proto);
    B("REDIRECT_PRESERVE_PATH", redirect_preserve_path);

    B("DNS_SPOOF", dns_spoof);
    S("DNS_SPOOF_FILE", dns_spoof_file);

    B("HONEYPOT", honeypot);
    S("HONEYPOT_PORTS", honeypot_ports);
    S("HONEYPOT_LOG", honeypot_log);
    S("HONEYPOT_BANNER", honeypot_banner);

    B("HID_ENABLED", hid_enabled);
    S("HID_LAYOUT", hid_layout);
    S("HID_PAYLOAD", hid_payload);
    S("HID_TRIGGER", hid_trigger);
    I("HID_DELAY_MS", hid_delay_ms);

    B("PAYLOADS", payloads);
    P("PAYLOAD_SWITCH1", payload_switch1);
    P("PAYLOAD_SWITCH2", payload_switch2);
    P("PAYLOAD_SWITCH3", payload_switch3);
    I("PAYLOAD_TAP_WINDOW_MS", payload_tap_window_ms);

    B("SSH", ssh);
    I("SSH_PORT", ssh_port);
    B("SSH_PASSWORD_AUTH", ssh_password_auth);
    B("SSH_ROOT", ssh_root);

    B("MDNS", mdns);
    S("MDNS_NAME", mdns_name);

    B("LOOT", loot);
    S("LOOT_WEBHOOK", loot_webhook);
    S("LOOT_WEBHOOK_SECRET", loot_webhook_secret);
    S("LOOT_PRIORITY", loot_priority);

    B("VPN", vpn);
    S("VPN_TYPE", vpn_type);
    S("VPN_CONFIG", vpn_config);

    B("SSL_STRIP", ssl_strip);
    S("MITM_CA", mitm_ca);

    B("WIFI", wifi);
    S("WIFI_MODE", wifi_mode);
    S("WIFI_SSID", wifi_ssid);
    S("WIFI_PASS", wifi_pass);
    S("WIFI_AP_SSID", wifi_ap_ssid);
    S("WIFI_AP_PASS", wifi_ap_pass);

    B("CRON", cron);
    S("CRON_FILE", cron_file);

    B("LED", led);
    S("LED_IDLE", led_idle);
    S("LED_RECORDING", led_recording);
    S("LED_ATTACK", led_attack);
    S("LED_WEBUI", led_webui);

    B("ALERTS", alerts);
    S("ALERT_WEBHOOK", alert_webhook);
    B("ALERT_ON_NEW_HOST", alert_on_new_host);
    B("ALERT_ON_RULE_HIT", alert_on_rule_hit);
    B("ALERT_ON_HONEYPOT", alert_on_honeypot);

    B("FLOWS", flows);
    I("FLOW_TIMEOUT_S", flow_timeout_s);
    I("FLOW_MAX", flow_max);

    B("RATE_LIMIT", rate_limit);
    S("RATE_LIMIT_TARGETS", rate_limit_targets);
    I("RATE_LIMIT_KBPS", rate_limit_kbps);

    S("RECORD_SCHEDULE", record_schedule);

    B("NTP", ntp);
    S("NTP_SERVERS", ntp_servers);

    B("WATCHDOG", watchdog);
    I("WATCHDOG_TIMEOUT_S", watchdog_timeout_s);

    I("PANIC_HOLD_S", panic_hold_s);
    B("PANIC_SCRUB", panic_scrub);

    B("OTA", ota);
    S("OTA_URL", ota_url);
    S("OTA_PUBKEY", ota_pubkey);

    S("LOG_LEVEL", log_level);
    B("LOG_TO_SERIAL", log_to_serial);
    S("LOG_TO_SD_FILE", log_to_sd_file);

    S("MODULES_ENABLED", modules_enabled);
    S("MODULES_DIR", modules_dir);

    B("SIBLING_BEACON", sibling_beacon);
    S("SIBLING_KEY", sibling_key);

    B("CAPTIVE_PORTAL", captive_portal);
    S("CAPTIVE_PORTAL_FILE", captive_portal_file);

    B("INJECT", inject);
    S("INJECT_FILE", inject_file);
    S("INJECT_MATCH", inject_match);

    B("ISOLATE_CLIENTS", isolate_clients);
    S("API_TOKEN", api_token);
    I("FACTORY_RESET_TAPS", factory_reset_taps);

#undef S
#undef B
#undef I
#undef H
#undef V
#undef P
    return -1; /* unknown key */
}

/* A '#' starts a comment only at the start of a value or after whitespace, so
 * passwords and URL fragments may contain '#'. This is the same rule as
 * onboard/ghostlink.sh, onboard/button.py and sdcard-template/ghostlink.sh. */
static void strip_comment(char *v) {
    if (*v == '#') { *v = 0; return; }
    for (char *p = v; p[0] && p[1]; p++) {
        if ((p[0] == ' ' || p[0] == '\t') && p[1] == '#') { p[0] = 0; return; }
    }
}

int cfg_load(const char *path) {
    cfg_set_default(&g_cfg);

    FILE *f = fopen(path, "r");
    if (!f) {
        LOGW("cfg", "no config at %s, using defaults", path);
        __atomic_add_fetch(&g_cfg_gen, 1, __ATOMIC_RELEASE);
        return -1;
    }

    char line[1024];
    int unknown = 0, parsed = 0, rejected = 0, lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *p = ltrim(line);
        if (*p == '#' || *p == '\n' || *p == 0 || *p == '\r') continue;

        char *eq = strchr(p, '=');
        if (!eq) {
            LOGW("cfg", "line %d: no '=' — ignored: %.60s", lineno, p);
            continue;
        }
        *eq = 0;
        char *key = p;
        char *val = eq + 1;

        strip_comment(val);

        rtrim(key);
        rtrim(val);
        /* kill trailing spaces on key */
        for (int i = (int)strlen(key) - 1; i >= 0 && (key[i] == ' ' || key[i] == '\t'); i--) key[i] = 0;
        val = ltrim(val);

        int rc = cfg_set_kv(&g_cfg, key, val);
        if (rc == 0) {
            parsed++;
        } else if (rc == -2) {
            rejected++;   /* cfg_set_kv already logged the line */
        } else {
            unknown++;
            LOGW("cfg", "line %d: unknown key '%s'", lineno, key);
        }
    }
    fclose(f);
    LOGI("cfg", "loaded %d keys (%d unknown, %d rejected) from %s",
         parsed, unknown, rejected, path);
    __atomic_add_fetch(&g_cfg_gen, 1, __ATOMIC_RELEASE);
    return 0;
}

/*
 * Atomic save: write to <path>.new then rename. Preserves comments is a goal
 * for a later patch; current implementation emits a canonical form.
 */
int cfg_save(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;

#define OB(k, field) fprintf(f, "%s=%s\n", k, g_cfg.field ? "TRUE" : "FALSE")
#define OS(k, field) fprintf(f, "%s=%s\n", k, g_cfg.field)
#define OI(k, field) fprintf(f, "%s=%d\n", k, g_cfg.field)
#define OH(k, field) fprintf(f, "%s=0x%04X\n", k, g_cfg.field)

    fprintf(f, "# GhostLink config (auto-written %lu)\n", (unsigned long)g_cfg_gen);
    OS("HOSTNAME", hostname);
    OS("DEVICE_NAME", device_name);
    OB("FAST_BOOT", fast_boot);
    OB("USB", usb);
    OS("USB_MODE", usb_mode);
    OH("USB_VID", usb_vid);
    OH("USB_PID", usb_pid);
    OB("USB_HID", usb_hid);
    OB("PASSTHROUGH_LAN", passthrough_lan);
    OS("NETMODE", netmode);
    OB("ASSIGN_IP", assign_ip);
    OS("DHCP_RANGE", dhcp_range);
    OS("GATEWAY_IP", gateway_ip);
    OS("DNS_UPSTREAM", dns_upstream);
    OB("LOG_TO_SD", log_to_sd);
    OI("RECORD_ROTATE_MB", record_rotate_mb);
    OI("RECORD_RING_FILES", record_ring_files);
    OB("VERDICT_STREAM", verdict_stream);
    OB("WEBUI", webui);
    OI("WEBUI_PORT", webui_port);
    OB("DEADNET", deadnet);
    OB("REDIRECT", redirect);
    OS("REDIRECT_FROM", redirect_from);
    OS("REDIRECT_TO", redirect_to);
    OB("DNS_SPOOF", dns_spoof);
    OB("HONEYPOT", honeypot);
    OB("HID_ENABLED", hid_enabled);
    OS("HID_LAYOUT", hid_layout);
    OS("HID_PAYLOAD", hid_payload);
    OB("SSH", ssh);
    OB("MDNS", mdns);
    OB("LOOT", loot);
    OB("WIFI", wifi);
    OB("CRON", cron);
    OB("LED", led);
    OB("ALERTS", alerts);
    OB("FLOWS", flows);
    OB("RATE_LIMIT", rate_limit);
    OB("NTP", ntp);
    OB("WATCHDOG", watchdog);
    OI("PANIC_HOLD_S", panic_hold_s);
    OB("OTA", ota);
    OS("LOG_LEVEL", log_level);
    OB("INJECT", inject);
    OB("ISOLATE_CLIENTS", isolate_clients);
    OI("FACTORY_RESET_TAPS", factory_reset_taps);

#undef OB
#undef OS
#undef OI
#undef OH
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(tmp, path) != 0) {
        LOGE("cfg", "rename %s -> %s failed: %s", tmp, path, strerror(errno));
        return -1;
    }
    __atomic_add_fetch(&g_cfg_gen, 1, __ATOMIC_RELEASE);
    LOGI("cfg", "saved to %s (gen %lu)", path, (unsigned long)g_cfg_gen);
    return 0;
}
