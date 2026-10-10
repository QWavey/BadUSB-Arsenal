/*
 * USB gadget via configfs. The whole function is one straight list of
 * write-to-file calls — no abstraction, because the kernel ABI IS the API.
 */

#include "gadget.h"
#include "../core/config.h"
#include "../common/log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define GROOT "/sys/kernel/config/usb_gadget/ghostlink"

static int wf(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) { LOGW("gadget", "open %s: %s", path, strerror(errno)); return -1; }
    size_t n = strlen(val);
    ssize_t w = write(fd, val, n);
    close(fd);
    if (w != (ssize_t)n) { LOGW("gadget", "write %s: short", path); return -1; }
    return 0;
}

static int wfprintf(const char *path, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return wf(path, buf);
}

static int mkd(const char *p) {
    if (mkdir(p, 0755) == 0) return 0;
    if (errno == EEXIST) return 0;
    LOGW("gadget", "mkdir %s: %s", p, strerror(errno));
    return -1;
}

/* Discover the UDC name (first file in /sys/class/udc). */
static int find_udc(char *out, size_t olen) {
    DIR *d = opendir("/sys/class/udc");
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(out, olen, "%s", e->d_name);
        closedir(d);
        return 0;
    }
    closedir(d);
    return -1;
}

/* HID keyboard boot-protocol report descriptor. */
static const unsigned char HID_KEYBOARD_DESC[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x03,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08,
    0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x03,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xc0
};

static int write_binary(const char *path, const void *buf, size_t n) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t w = write(fd, buf, n);
    close(fd);
    return (w == (ssize_t)n) ? 0 : -1;
}

int gadget_start(void) {
    if (!g_cfg.usb) { LOGI("gadget", "USB disabled"); return 0; }

    /* Ensure configfs mounted. */
    mkdir("/sys/kernel/config", 0755);
    if (mount("none", "/sys/kernel/config", "configfs", 0, NULL) < 0 && errno != EBUSY) {
        LOGW("gadget", "configfs mount: %s", strerror(errno));
    }

    /* Load module(s) if present. Ignore failures (compiled-in on most kernels). */
    system("modprobe libcomposite >/dev/null 2>&1");

    mkd("/sys/kernel/config/usb_gadget");
    mkd(GROOT);

    wfprintf(GROOT "/idVendor",  "0x%04X", g_cfg.usb_vid);
    wfprintf(GROOT "/idProduct", "0x%04X", g_cfg.usb_pid);
    wfprintf(GROOT "/bcdDevice", "0x0100");
    wfprintf(GROOT "/bcdUSB",    "0x0200");

    mkd(GROOT "/strings/0x409");
    char serial[32];
    if (strcmp(g_cfg.usb_serial, "AUTO") == 0) {
        /* RV1106 chip id lives at /sys/firmware/devicetree/.../cpu0 — fallback: /proc/cpuinfo Serial */
        FILE *f = fopen("/proc/cpuinfo", "r");
        serial[0] = 0;
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "Serial", 6) == 0) {
                    char *c = strchr(line, ':');
                    if (c) { c++; while (*c == ' ') c++;
                             strncpy(serial, c, sizeof(serial)-1);
                             serial[sizeof(serial)-1] = 0;
                             size_t L = strlen(serial);
                             if (L && serial[L-1] == '\n') serial[L-1] = 0; }
                    break;
                }
            }
            fclose(f);
        }
        if (!serial[0]) snprintf(serial, sizeof(serial), "GHOST%08X", (unsigned)getpid());
    } else {
        strncpy(serial, g_cfg.usb_serial, sizeof(serial)-1);
        serial[sizeof(serial)-1] = 0;
    }
    wf(GROOT "/strings/0x409/serialnumber", serial);
    wf(GROOT "/strings/0x409/manufacturer", g_cfg.usb_manufacturer);
    wf(GROOT "/strings/0x409/product",       g_cfg.usb_product);

    mkd(GROOT "/configs/c.1");
    mkd(GROOT "/configs/c.1/strings/0x409");
    wf(GROOT "/configs/c.1/strings/0x409/configuration", "GhostLink");
    wfprintf(GROOT "/configs/c.1/MaxPower", "250");

    /* --- NCM (preferred) or RNDIS --- */
    int want_net = (strcasecmp(g_cfg.usb_mode, "NCM")   == 0 ||
                    strcasecmp(g_cfg.usb_mode, "RNDIS") == 0 ||
                    strcasecmp(g_cfg.usb_mode, "COMPOSITE") == 0);
    if (want_net) {
        int rndis = (strcasecmp(g_cfg.usb_mode, "RNDIS") == 0);
        const char *fname = rndis ? "rndis.usb0" : "ncm.usb0";
        char fpath[256]; snprintf(fpath, sizeof(fpath), GROOT "/functions/%s", fname);
        mkd(fpath);
        /* Random but stable MACs so DHCP leases survive reboot. */
        char mac_host[18], mac_dev[18];
        snprintf(mac_host, sizeof(mac_host), "02:00:60:0D:%02X:%02X",
                 (unsigned)(serial[0] ^ 0x5A) & 0xFF, (unsigned)(serial[1] ^ 0xA5) & 0xFF);
        snprintf(mac_dev,  sizeof(mac_dev),  "02:00:60:0D:%02X:%02X",
                 (unsigned)(serial[2] ^ 0x5A) & 0xFF, (unsigned)(serial[3] ^ 0xA5) & 0xFF);
        char kp[320];
        snprintf(kp, sizeof(kp), "%s/host_addr", fpath); wf(kp, mac_host);
        snprintf(kp, sizeof(kp), "%s/dev_addr",  fpath); wf(kp, mac_dev);

        char link[320], target[320];
        snprintf(link, sizeof(link),   GROOT "/configs/c.1/%s", fname);
        snprintf(target, sizeof(target), "../functions/%s", fname);
        symlink(target, link);
    }

    /* --- HID keyboard --- */
    if (g_cfg.usb_hid) {
        mkd(GROOT "/functions/hid.usb0");
        wfprintf(GROOT "/functions/hid.usb0/protocol",   "1"); /* keyboard */
        wfprintf(GROOT "/functions/hid.usb0/subclass",   "1"); /* boot */
        wfprintf(GROOT "/functions/hid.usb0/report_length", "8");
        write_binary(GROOT "/functions/hid.usb0/report_desc",
                     HID_KEYBOARD_DESC, sizeof(HID_KEYBOARD_DESC));
        symlink("../functions/hid.usb0", GROOT "/configs/c.1/hid.usb0");
    }

    /* --- Mass storage --- */
    if (g_cfg.usb_storage) {
        mkd(GROOT "/functions/mass_storage.usb0");
        wf(GROOT "/functions/mass_storage.usb0/lun.0/file", "/dev/mmcblk0");
        wf(GROOT "/functions/mass_storage.usb0/lun.0/ro",   "0");
        wf(GROOT "/functions/mass_storage.usb0/lun.0/removable", "1");
        symlink("../functions/mass_storage.usb0", GROOT "/configs/c.1/mass_storage.usb0");
    }

    /* --- Bind UDC --- */
    char udc[64];
    if (find_udc(udc, sizeof(udc)) != 0) {
        LOGE("gadget", "no UDC found in /sys/class/udc");
        return -1;
    }
    LOGI("gadget", "binding to UDC %s", udc);
    if (wf(GROOT "/UDC", udc) != 0) {
        LOGE("gadget", "UDC bind failed");
        return -1;
    }
    LOGI("gadget", "USB gadget online (vid=%04X pid=%04X mode=%s hid=%d)",
         g_cfg.usb_vid, g_cfg.usb_pid, g_cfg.usb_mode, g_cfg.usb_hid);
    return 0;
}

int gadget_stop(void) {
    wf(GROOT "/UDC", "");
    return 0;
}
