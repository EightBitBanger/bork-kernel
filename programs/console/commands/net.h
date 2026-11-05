#ifndef CONSOLE_CMD_NET_H
#define CONSOLE_CMD_NET_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <os/os.h>
#include <os/security.h>
#include "console_host.h"

// net                  wireless adapters
// net scan [wlanN]     listen for networks, then list them
// net list [wlanN]     the last scan's networks again
//
//   SSID                              BSSID              CH  SIGNAL   SECURITY
//   HomeNet                           a4:2b:b0:11:22:33   6  -48 dBm  WPA2
//
// Through /dev/wlanN, which needs the network permission (SEC_NET). A
// console started from the desktop doesn't have it: net asks the user for
// administrator rights once (the desktop's prompt), gives this console the
// network bit, and hands administrator back at once. Programs this console
// starts still don't get the network bit.

#ifndef SEC_NET
#define SEC_NET                 0x00000400u     // libc older than the TCP update
#endif

// A copy of the kernel's drivers/net/wlan.h ABI, which must stay the same
#define WLAN_IOCTL_INFO         0x574C0001u
#define WLAN_IOCTL_SCAN         0x574C0002u
#define WLAN_IOCTL_SCAN_RESULTS 0x574C0003u
#define WLAN_SCAN_VERSION       1
#define WLAN_SCAN_MAX           32
#define WLAN_SSID_MAX           32
#define WLAN_STATE_PROBED       1
#define WLAN_STATE_IDENTIFIED   2
#define WLAN_STATE_NO_EFUSE     3
#define WLAN_STATE_GONE         4
#define WLAN_STATE_RADIO_ON     5
#define WLAN_STATE_RADIO_FAILED 6
#define WLAN_STATE_RADIO_OFF    7
#define WLAN_SCAN_IDLE          0
#define WLAN_SCAN_RUNNING       1
#define WLAN_SCAN_DONE          2
#define WLAN_SCAN_FAILED        3
#define WLAN_SEC_WEP            0x01
#define WLAN_SEC_WPA            0x02
#define WLAN_SEC_WPA2           0x04
#define WLAN_SEC_WPA3           0x08
#define WLAN_SEC_ENTERPRISE     0x10
#define WLAN_BSS_IBSS           0x04

struct WlanInfo {
    uint32_t version;
    uint16_t vendor, product;
    uint8_t  family, state, usb_speed, mac_valid;
    uint8_t  mac[6];
    uint8_t  chip_cut, chip_id;
    uint32_t sys_cfg;
    char     chip[16];
};

struct WlanBss {
    uint8_t  bssid[6];
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  security;
    uint8_t  flags;
    uint16_t beacon_interval;
    uint8_t  ssid_length;
    uint8_t  ssid[WLAN_SSID_MAX];
    uint8_t  reserved[3];
};

struct WlanScanResults {
    uint32_t version;
    uint8_t  state, channel, count, firmware;
    struct WlanBss bss[WLAN_SCAN_MAX];
};

_Static_assert(sizeof(struct WlanInfo) == 40 && sizeof(struct WlanBss) == 48 &&
               sizeof(struct WlanScanResults) == 8 + 48 * WLAN_SCAN_MAX, "wlan.h ABI");

// Formatted text to the log (print() is the console's only output)
#define NET_PRINTF(...) do { char net_line_[128]; snprintf(net_line_, sizeof(net_line_), __VA_ARGS__); print(net_line_); } while (0)

#define NET_SCAN_TIMEOUT_MS     20000
#define NET_SCAN_POLL_MS        200

static const char* net_wlan_state(uint8_t state) {
    switch (state) {
        case WLAN_STATE_PROBED:       return "identifying";
        case WLAN_STATE_IDENTIFIED:   return "ready";
        case WLAN_STATE_NO_EFUSE:     return "ready (no MAC)";
        case WLAN_STATE_GONE:         return "unplugged";
        case WLAN_STATE_RADIO_ON:     return "radio on";
        case WLAN_STATE_RADIO_FAILED: return "radio failed";
        case WLAN_STATE_RADIO_OFF:    return "radio off (wireless switch)";
        default:                      return "?";
    }
}

// Get the network bit if we lack it: once, through the desktop's prompt
static bool net_permission(void) {
    if (sec_has(SEC_NET))
        return true;
    if (!sec_has(SEC_ELEVATE)) {
        print("net: this console has no network permission\n");
        return false;
    }
    bool was_admin = sec_is_admin();
    if (!was_admin && sec_elevate("Give this console network access (list Wi-Fi networks)") < 0) {
        print(errno == EACCES ? "net: cancelled\n" : "net: can't ask for network permission\n");
        return false;
    }
    bool ok = sec_grant(0, SEC_NET) == 0;
    if (!was_admin)
        sec_drop(SEC_ADMIN);
    if (!ok)
        print("net: could not take the network permission\n");
    return ok;
}

// /dev/wlan<index>, or -1 (message printed)
static int net_open(uint32_t index, bool quiet) {
    char path[16];
    snprintf(path, sizeof(path), "/dev/wlan%u", (unsigned)index);
    int fd = open(path, O_RDONLY);
    if (fd < 0 && !quiet) {
        if (errno == ENOENT)
            NET_PRINTF("net: no %s\n", path + 5);
        else
            NET_PRINTF("net: %s: %s\n", path, strerror(errno));
    }
    return fd;
}

// Bytes off the air: printable ASCII as is, anything else as \xNN, so a
// network name can't move the cursor or send terminal escapes
static void net_print_ssid(const struct WlanBss* b, size_t width) {
    char out[4 * WLAN_SSID_MAX + 1];
    size_t n = 0;
    if (b->ssid_length == 0) {
        strcpy(out, "(hidden)");
        n = strlen(out);
    }
    for (uint32_t i = 0; i < b->ssid_length && i < WLAN_SSID_MAX; i++) {
        uint8_t c = b->ssid[i];
        if (c >= 0x20 && c < 0x7F && c != '\\') {
            out[n++] = (char)c;
        } else {
            static const char hex[] = "0123456789abcdef";
            out[n++] = '\\';
            out[n++] = 'x';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = '\0';
    print(out);
    for (; n < width; n++)
        print(" ");
}

static void net_print_security(uint8_t sec) {
    if (sec == 0)            { print("open"); return; }
    if (sec == WLAN_SEC_WEP) { print("WEP"); return; }
    bool first = true;
    if (sec & WLAN_SEC_WPA3) { print("WPA3"); first = false; }
    if (sec & WLAN_SEC_WPA2) { print(first ? "WPA2" : "/WPA2"); first = false; }
    if (sec & WLAN_SEC_WPA)  { print(first ? "WPA" : "/WPA"); first = false; }
    if (sec & WLAN_SEC_ENTERPRISE) print("-EAP");
}

static void net_print_results(uint32_t index, const struct WlanScanResults* r) {
    char line[96];
    snprintf(line, sizeof(line), "wlan%u: %u network%s%s\n", (unsigned)index, (unsigned)r->count,
             r->count == 1 ? "" : "s", r->firmware ? "" : " (no firmware loaded)");
    print(line);
    if (r->count == 0)
        return;
    print("  SSID                              BSSID              CH  SIGNAL   SECURITY\n");
    for (uint32_t i = 0; i < r->count && i < WLAN_SCAN_MAX; i++) {
        const struct WlanBss* b = &r->bss[i];
        print("  ");
        net_print_ssid(b, 34);
        const uint8_t* m = b->bssid;
        if (b->rssi <= -127)
            snprintf(line, sizeof(line), "%02x:%02x:%02x:%02x:%02x:%02x  %2u    ?     ",
                     m[0], m[1], m[2], m[3], m[4], m[5], (unsigned)b->channel);
        else
            snprintf(line, sizeof(line), "%02x:%02x:%02x:%02x:%02x:%02x  %2u  %4d dBm ",
                     m[0], m[1], m[2], m[3], m[4], m[5], (unsigned)b->channel, (int)b->rssi);
        print(line);
        net_print_security(b->security);
        if (b->flags & WLAN_BSS_IBSS)
            print(" ad-hoc");
        print("\n");
    }
}

static int net_list_adapters(void) {
    bool any = false;
    for (uint32_t i = 0; i < 10; i++) {
        int fd = net_open(i, true);
        if (fd < 0)
            continue;
        struct WlanInfo info;
        memset(&info, 0, sizeof(info));
        if (ioctl(fd, WLAN_IOCTL_INFO, &info) == 0) {
            char line[96];
            info.chip[sizeof(info.chip) - 1] = '\0';
            if (info.mac_valid)
                snprintf(line, sizeof(line), "wlan%u  %s  %02x:%02x:%02x:%02x:%02x:%02x  %s\n", (unsigned)i, info.chip,
                         info.mac[0], info.mac[1], info.mac[2], info.mac[3], info.mac[4], info.mac[5],
                         net_wlan_state(info.state));
            else
                snprintf(line, sizeof(line), "wlan%u  %s  %s\n", (unsigned)i, info.chip, net_wlan_state(info.state));
            print(line);
            any = true;
        }
        close(fd);
    }
    if (!any)
        print("no wireless adapters\n");
    print("net scan [wlanN] lists the networks in range\n");
    return any ? 0 : 1;
}

static int net_scan_error(int e) {
    switch (e) {
        case ENOTTY: print("net: this chip can't scan yet (RTL8192EU and AR9485 only)\n"); break;
        case EIO:    print("net: the adapter is not working (see the kernel log)\n"); break;
        case EAGAIN: print("net: still identifying the adapter, try again in a moment\n"); break;
        case EBUSY:  print("net: a scan is already running\n"); break;
        default:     NET_PRINTF("net: %s\n", strerror(e)); break;
    }
    return 1;
}

static int net_run(int arg_count, char** args) {
    if (!net_permission())
        return 1;
    if (arg_count == 0)
        return net_list_adapters();

    uint32_t index = 0;
    if (arg_count >= 2) {
        const char* t = args[1];
        if (strncmp(t, "wlan", 4) == 0)
            t += 4;
        if (t[0] < '0' || t[0] > '9' || t[1] != '\0') {
            print("usage: net [scan | list] [wlanN]\n");
            return 1;
        }
        index = (uint32_t)(t[0] - '0');
    }
    bool scan = strcmp(args[0], "scan") == 0;
    if (!scan && strcmp(args[0], "list") != 0) {
        print("usage: net [scan | list] [wlanN]\n");
        return 1;
    }

    int fd = net_open(index, false);
    if (fd < 0)
        return 1;
    static struct WlanScanResults r;    // 1.5 KB
    int result = 0;

    if (scan) {
        if (ioctl(fd, WLAN_IOCTL_SCAN, 0) < 0) {
            int e = errno;
            close(fd);
            return net_scan_error(e);
        }
        NET_PRINTF("scanning wlan%u (channels 1-13, listening only)...\n", (unsigned)index);
        console_flush();
        uint32_t waited = 0;
        while (1) {
            os_sleep_ms(NET_SCAN_POLL_MS);
            waited += NET_SCAN_POLL_MS;
            if (ioctl(fd, WLAN_IOCTL_SCAN_RESULTS, &r) < 0) {
                NET_PRINTF("net: %s\n", strerror(errno));
                close(fd);
                return 1;
            }
            if (r.state != WLAN_SCAN_RUNNING)
                break;
            if (waited >= NET_SCAN_TIMEOUT_MS) {
                print("still scanning; 'net list' shows what it found so far\n");
                break;
            }
        }
    } else {
        if (ioctl(fd, WLAN_IOCTL_SCAN_RESULTS, &r) < 0) {
            NET_PRINTF("net: %s\n", strerror(errno));
            close(fd);
            return 1;
        }
        if (r.state == WLAN_SCAN_IDLE) {
            print("no scan yet: net scan\n");
            close(fd);
            return 0;
        }
        if (r.state == WLAN_SCAN_RUNNING)
            print("(scan still running)\n");
    }
    close(fd);

    if (r.version != WLAN_SCAN_VERSION) {
        print("net: the kernel's scan format is newer than this console\n");
        return 1;
    }
    if (r.state == WLAN_SCAN_FAILED) {
        print("scan failed: the radio did not come up (see the kernel log)\n");
        return 1;
    }
    if (r.count > WLAN_SCAN_MAX)
        r.count = WLAN_SCAN_MAX;
    net_print_results(index, &r);
    return result;
}

int console_cmd_net(int arg_count, char** args) {
    int result = net_run(arg_count, args);
    console_command_end();
    return result;
}

#endif
