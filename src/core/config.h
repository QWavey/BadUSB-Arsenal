#ifndef GHOST_CONFIG_H
#define GHOST_CONFIG_H

#include <stdint.h>
#include <stddef.h>

/*
 * Config schema. One canonical structure every module reads from.
 * Values are populated by cfg_load() parsing config.txt.
 * Hot-reload rewrites fields atomically under a shared RW lock.
 */

typedef struct {
    /* 1. core */
    char hostname[64];
    char device_name[64];
    char timezone[32];
    char locale[32];
    int  fast_boot;
    int  boot_led;

    /* 2. usb */
    int  usb;
    char usb_mode[16];     /* NCM|RNDIS|HID|STORAGE|COMPOSITE */
    uint16_t usb_vid;
    uint16_t usb_pid;
    char usb_manufacturer[64];
    char usb_product[64];
    char usb_serial[32];
    int  usb_hid;
    int  usb_storage;

    /* 3. net */
    int  passthrough_lan;
    char netmode[16];      /* BRIDGE|NAT|TRANSPARENT|VPN|ISOLATE */
    int  assign_ip;
    char dhcp_range[64];
    char gateway_ip[32];
    char dns_upstream[128];
    int  mac_clone;
    char mac_clone_from[16];
    char static_ip[32];
    char static_netmask[32];
    char static_gateway[32];

    /* 4. record */
    int  log_to_sd;
    char record_format[16];
    int  record_rotate_mb;
    int  record_ring_files;
    char record_filter[256];
    int  verdict_stream;
    int  verdict_include_payload;

    /* 5. webui */
    int  webui;
    int  webui_port;
    char webui_bind[32];
    char webui_user[32];
    char webui_pass[64];
    char webui_lang[8];

    /* 6. deadnet */
    int  deadnet;
    int  deadnet_ipv4;
    int  deadnet_ipv6;
    int  deadnet_interval_ms;
    char deadnet_spare[256];
    int  deadnet_selective;
    char deadnet_targets[256];

    /* 7. redirect */
    int  redirect;
    char redirect_from[512];
    char redirect_to[256];
    char redirect_proto[16];
    int  redirect_preserve_path;

    /* 8. dns spoof */
    int  dns_spoof;
    char dns_spoof_file[128];

    /* 9. honeypot */
    int  honeypot;
    char honeypot_ports[128];
    char honeypot_log[128];
    char honeypot_banner[128];

    /* 10. hid */
    int  hid_enabled;
    char hid_layout[16];
    char hid_payload[128];
    char hid_trigger[16];
    int  hid_delay_ms;

    /* 11. payloads */
    int  payloads;
    char payload_switch1[128];
    char payload_switch2[128];
    char payload_switch3[128];
    int  payload_tap_window_ms;

    /* 12. ssh */
    int  ssh;
    int  ssh_port;
    int  ssh_password_auth;
    int  ssh_root;

    /* 13. mdns */
    int  mdns;
    char mdns_name[64];

    /* 14. loot */
    int  loot;
    char loot_webhook[256];
    char loot_webhook_secret[128];
    char loot_priority[16];

    /* 15. vpn */
    int  vpn;
    char vpn_type[16];
    char vpn_config[128];

    /* 16. ssl strip */
    int  ssl_strip;
    char mitm_ca[128];

    /* 17. wifi */
    int  wifi;
    char wifi_mode[16];
    char wifi_ssid[64];
    char wifi_pass[64];
    char wifi_ap_ssid[64];
    char wifi_ap_pass[64];

    /* 18. cron */
    int  cron;
    char cron_file[128];

    /* 19. led */
    int  led;
    char led_idle[32];
    char led_recording[32];
    char led_attack[32];
    char led_webui[32];

    /* 20. alerts */
    int  alerts;
    char alert_webhook[256];
    int  alert_on_new_host;
    int  alert_on_rule_hit;
    int  alert_on_honeypot;

    /* 21. flows */
    int  flows;
    int  flow_timeout_s;
    int  flow_max;

    /* 22. rate limit */
    int  rate_limit;
    char rate_limit_targets[256];
    int  rate_limit_kbps;

    /* 23. schedule record */
    char record_schedule[128];

    /* 24. ntp */
    int  ntp;
    char ntp_servers[128];

    /* 25. watchdog */
    int  watchdog;
    int  watchdog_timeout_s;

    /* 26. panic */
    int  panic_hold_s;
    int  panic_scrub;

    /* 27. ota */
    int  ota;
    char ota_url[256];
    char ota_pubkey[128];

    /* 28. logging */
    char log_level[16];
    int  log_to_serial;
    char log_to_sd_file[128];

    /* 29. modules */
    char modules_enabled[256];
    char modules_dir[128];

    /* 30. sibling */
    int  sibling_beacon;
    char sibling_key[64];

    /* 31. captive portal */
    int  captive_portal;
    char captive_portal_file[128];

    /* 32. inject */
    int  inject;
    char inject_file[128];
    char inject_match[128];

    /* 33. isolate */
    int  isolate_clients;

    /* 34. api token */
    char api_token[128];

    /* 35. factory reset */
    int  factory_reset_taps;

} ghost_cfg_t;

/* The one global the whole firmware looks at. */
extern ghost_cfg_t g_cfg;

int  cfg_load(const char *path);
int  cfg_save(const char *path);
void cfg_set_default(ghost_cfg_t *c);
int  cfg_set_kv(ghost_cfg_t *c, const char *key, const char *val);
const char *cfg_get(const ghost_cfg_t *c, const char *key, char *out, size_t olen);

/* generation counter: bumps every time cfg_load reruns, so services can
 * poll and hot-reload without IPC. */
extern volatile uint64_t g_cfg_gen;

#endif
