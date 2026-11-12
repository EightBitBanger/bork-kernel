#ifndef CONSOLE_CMD_NET_H
#define CONSOLE_CMD_NET_H

#include <stdint.h>
#include <kernel/console/print.h>
#include <kernel/net/net.h>
#include <kernel/arch/x86/drivers/net/wlan.h>
#include <kernel/syscall/syscall.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/util/string.h>
#include <kernel/malloc.h>

// ifconfig                          list interfaces
// ifconfig <if> <ip>/<bits> [gw]    static address
// ifconfig <if> dhcp | none         ask DHCP for one / drop it
// ifconfig <if> up | down
// ping <ip> [count]                 ICMP echo, one a second
// arp                               the neighbour table
// net                               wireless adapters
// net scan [wlanN]                  listen for networks, then list them
// net list [wlanN]                  the last scan's networks again
//
//   wlan0  up  no-link  dhcp  00:14:cc:20:81:92  mtu 1500
//          inet 192.168.1.20/24  gw 192.168.1.1
//          rx 0 pkts 0 bytes 0 dropped  tx 0 pkts 0 bytes 0 dropped

static void console_net_uint(uint32_t value) {
    char text[12];
    utos(value, text);
    print(text);
}

static void console_net_ip(uint32_t ip) {
    char text[16];
    net_format_ip(ip, text);
    print(text);
}

static uint32_t console_net_prefix(uint32_t mask) {
    uint32_t bits = 0;
    while (bits < 32 && (mask & (0x80000000u >> bits)))
        bits++;
    return bits;
}

static void console_net_error(int code) {
    switch (code) {
        case NET_ERR_PARAM:   print("bad argument\n"); break;
        case NET_ERR_DOWN:    print("interface down or no link\n"); break;
        case NET_ERR_NOROUTE: print("no route to host\n"); break;
        case NET_ERR_TIMEOUT: print("timed out\n"); break;
        case NET_ERR_AGAIN:   print("busy, try again\n"); break;
        case NET_ERR_TOOBIG:  print("too big\n"); break;
        case NET_ERR_NOMEM:   print("out of memory\n"); break;
        default:              print("failed\n"); break;
    }
}

static void console_ifconfig_show(const struct NetIf* ifp) {
    print(ifp->name);
    for (size_t n = strlen(ifp->name); n < 7; n++) print(" ");
    print(ifp->flags & NETIF_UP ? "up  " : "down  ");
    print(ifp->flags & NETIF_LINK ? "link  " : "no-link  ");
    if (ifp->flags & NETIF_DHCP) print("dhcp  ");
    if (!(ifp->flags & NETIF_LOOPBACK)) {
        char mac[18];
        net_format_mac(ifp->mac, mac);
        print(mac);
        print("  ");
    }
    print("mtu ");
    console_net_uint(ifp->mtu);
    print("\n       inet ");
    if (ifp->ip == 0) {
        print("none");
    } else {
        console_net_ip(ifp->ip);
        print("/");
        console_net_uint(console_net_prefix(ifp->netmask));
        if (ifp->gateway != 0) {
            print("  gw ");
            console_net_ip(ifp->gateway);
        }
        if (ifp->dns != 0) {
            print("  dns ");
            console_net_ip(ifp->dns);
        }
    }
    const struct NetIfStats* s = &ifp->stats;
    print("\n       rx ");
    console_net_uint(s->rx_packets); print(" pkts ");
    console_net_uint(s->rx_bytes);   print(" bytes ");
    console_net_uint(s->rx_dropped); print(" dropped  tx ");
    console_net_uint(s->tx_packets); print(" pkts ");
    console_net_uint(s->tx_bytes);   print(" bytes ");
    console_net_uint(s->tx_dropped); print(" dropped\n");
}

static int cmd_ifconfig_run(int arg_count, char** args) {
    if (arg_count == 0) {
        struct NetIf ifp;
        for (uint32_t i = 0; netif_snapshot(i, &ifp); i++)
            console_ifconfig_show(&ifp);
        return 0;
    }
    if (arg_count < 2) {
        print("ifconfig <if> <ip>/<bits> [gw] | dhcp | none | up | down\n");
        return 1;
    }

    const char* name = args[0];
    const char* what = args[1];
    int result;
    if (strcmp(what, "up") == 0 || strcmp(what, "down") == 0) {
        result = netif_set_up(name, what[0] == 'u');
    } else if (strcmp(what, "dhcp") == 0) {
        result = netif_dhcp(name);
    } else if (strcmp(what, "none") == 0) {
        result = netif_configure(name, 0, 0, 0);
    } else {
        // a.b.c.d/bits
        char ip_text[16];
        const char* slash = strchr(what, '/');
        size_t ip_length = slash != NULL ? (size_t)(slash - what) : strlen(what);
        uint32_t ip = 0, gateway = 0, bits = 24;
        bool ok = ip_length < sizeof(ip_text);
        if (ok) {
            memcpy(ip_text, what, ip_length);
            ip_text[ip_length] = '\0';
            ok = net_parse_ip(ip_text, &ip);
        }
        if (ok && slash != NULL) {
            const char* b = slash + 1;
            ok = b[0] >= '0' && b[0] <= '9' && strlen(b) <= 2;
            for (const char* c = b; ok && *c != '\0'; c++)
                ok = *c >= '0' && *c <= '9';
            if (ok) bits = stou(b);
            ok = ok && bits >= 1 && bits <= 30;
        }
        if (ok && arg_count >= 3)
            ok = net_parse_ip(args[2], &gateway);
        if (!ok) {
            print("ifconfig: expected <ip>/<1-30> [gateway]\n");
            return 1;
        }
        uint32_t mask = 0xFFFFFFFFu << (32 - bits);
        result = netif_configure(name, ip, mask, gateway);
    }

    if (result != NET_OK) {
        print("ifconfig: ");
        print(name);
        print(": ");
        console_net_error(result);
        return 2;
    }
    return 0;
}

int console_cmd_ifconfig(int arg_count, char** args) {
    int result = cmd_ifconfig_run(arg_count, args);
    console_command_end();
    return result;
}

static int cmd_ping_run(int arg_count, char** args) {
    uint32_t ip;
    if (arg_count < 1 || !net_parse_ip(args[0], &ip)) {
        print("ping <a.b.c.d> [count]\n");
        return 1;
    }
    uint32_t count = 4;
    if (arg_count >= 2) {
        count = stou(args[1]);
        if (count == 0 || count > 100) count = 4;
    }

    uint32_t received = 0;
    for (uint32_t i = 0; i < count; i++) {
        int rtt = net_ping(ip, (uint16_t)(i + 1), 1000);
        print("seq ");
        console_net_uint(i + 1);
        print(": ");
        if (rtt >= 0) {
            received++;
            print("reply from ");
            console_net_ip(ip);
            print(" in ");
            console_net_uint((uint32_t)rtt);
            print(" ms\n");
        } else {
            console_net_error(rtt);
            if (rtt != NET_ERR_TIMEOUT)
                break;  // No route / down: the rest would say the same
        }
        if (rtt >= 0 && rtt < 1000 && i + 1 < count)
            thread_sleep(1000 - (uint32_t)rtt);
    }
    console_net_uint(received);
    print(" of ");
    console_net_uint(count);
    print(" answered\n");
    return received > 0 ? 0 : 2;
}

int console_cmd_ping(int arg_count, char** args) {
    int result = cmd_ping_run(arg_count, args);
    console_command_end();
    return result;
}

static int cmd_arp_run(int arg_count, char** args) {
    (void)arg_count; (void)args;
    struct ArpView entries[16];
    uint32_t count = arp_snapshot(entries, 16);
    if (count == 0) {
        print("ARP table empty\n");
        return 0;
    }
    for (uint32_t i = 0; i < count; i++) {
        char text[18];
        net_format_ip(entries[i].ip, text);
        print(text);
        for (size_t n = strlen(text); n < 17; n++) print(" ");
        if (entries[i].complete) {
            net_format_mac(entries[i].mac, text);
            print(text);
        } else {
            print("(waiting)        ");
        }
        print("  ");
        print(entries[i].ifname);
        print("\n");
    }
    return 0;
}

int console_cmd_arp(int arg_count, char** args) {
    int result = cmd_arp_run(arg_count, args);
    console_command_end();
    return result;
}

//
// net: wireless adapters and scanning
//

#define NET_SCAN_TIMEOUT_MS     20000
#define NET_SCAN_POLL_MS        200

static const char* console_wlan_state(uint8_t state) {
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

static void console_wlan_error(int32_t code) {
    switch (code) {
        case SYS_ENOENT: print("no such adapter\n"); break;
        case SYS_ENOTTY: print("this chip can't scan yet (RTL8192EU and AR9485 only)\n"); break;
        case SYS_EIO:    print("the adapter is not working (see the boot log)\n"); break;
        case SYS_EAGAIN: print("still identifying the adapter, try again in a moment\n"); break;
        case SYS_EBUSY:  print("a scan is already running\n"); break;
        default:         print("failed\n"); break;
    }
}

// Bytes off the air: printable ASCII as is, everything else as \xNN, so a
// name can't move the cursor or send the terminal escapes. Pads to `width`.
static void console_wlan_ssid(const struct WlanBss* b, uint32_t width) {
    char out[4 * WLAN_SSID_MAX + 1];
    uint32_t n = 0;
    if (b->ssid_length == 0) {
        const char* hidden = "(hidden)";
        while (*hidden) out[n++] = *hidden++;
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
    for (; n < width; n++) print(" ");
}

static void console_wlan_security(uint8_t sec) {
    if (sec == 0)                    { print("open"); return; }
    if (sec == WLAN_SEC_WEP)         { print("WEP"); return; }
    bool first = true;
    if (sec & WLAN_SEC_WPA3) { print("WPA3"); first = false; }
    if (sec & WLAN_SEC_WPA2) { print(first ? "WPA2" : "/WPA2"); first = false; }
    if (sec & WLAN_SEC_WPA)  { print(first ? "WPA" : "/WPA"); first = false; }
    if (sec & WLAN_SEC_ENTERPRISE) print("-EAP");
}

static void console_wlan_show_results(uint32_t index, const struct WlanScanResults* r) {
    print("wlan");
    console_net_uint(index);
    print(": ");
    console_net_uint(r->count);
    print(r->count == 1 ? " network" : " networks");
    if (!r->firmware) print(" (no firmware loaded)");
    print("\n");
    if (r->count == 0)
        return;
    print("  SSID                              BSSID              CH  SIGNAL   SECURITY\n");
    for (uint32_t i = 0; i < r->count && i < WLAN_SCAN_MAX; i++) {
        const struct WlanBss* b = &r->bss[i];
        print("  ");
        console_wlan_ssid(b, 34);
        char mac[18];
        net_format_mac(b->bssid, mac);
        print(mac);
        print("  ");
        if (b->channel < 10) print(" ");
        console_net_uint(b->channel);
        print("  ");
        if (b->rssi <= -127) {
            print("  ?      ");
        } else {
            char text[12];
            int v = -(int)b->rssi;
            utos((uint32_t)v, text);
            print(v < 10 ? "  -" : (v < 100 ? " -" : "-"));
            print(text);
            print(" dBm ");
        }
        console_wlan_security(b->security);
        if (b->flags & WLAN_BSS_IBSS) print(" ad-hoc");
        print("\n");
    }
}

// "wlan1" or "1" -> 1; no argument: 0
static bool console_wlan_index(int arg_count, char** args, uint32_t* index) {
    *index = 0;
    if (arg_count < 2)
        return true;
    const char* t = args[1];
    if (strncmp(t, "wlan", 4) == 0)
        t += 4;
    if (t[0] < '0' || t[0] > '9' || t[1] != '\0')
        return false;
    *index = (uint32_t)(t[0] - '0');
    return true;
}

static int cmd_net_run(int arg_count, char** args) {
    if (arg_count == 0) {
        bool any = false;
        for (uint32_t i = 0; i < 10; i++) {
            struct WlanInfo info;
            if (wlan_info(i, &info) != SYS_OK)
                continue;
            any = true;
            print("wlan");
            console_net_uint(i);
            print("  ");
            print(info.chip);
            print("  ");
            if (info.mac_valid) {
                char mac[18];
                net_format_mac(info.mac, mac);
                print(mac);
                print("  ");
            }
            print(console_wlan_state(info.state));
            print("\n");
        }
        if (!any)
            print("no wireless adapters\n");
        print("net scan [wlanN] lists the networks in range\n");
        return any ? 0 : 1;
    }

    uint32_t index;
    if (!console_wlan_index(arg_count, args, &index)) {
        print("usage: net [scan | list] [wlanN]\n");
        return 1;
    }

    struct WlanScanResults* r = (struct WlanScanResults*)malloc(sizeof(struct WlanScanResults));
    if (r == NULL) {
        print("out of memory\n");
        return 1;
    }
    int result = 0;

    if (strcmp(args[0], "scan") == 0) {
        int32_t e = wlan_scan_start(index);
        if (e != SYS_OK) {
            console_wlan_error(e);
            free(r);
            return 1;
        }
        print("scanning wlan");
        console_net_uint(index);
        print(" (channels 1-13, listening only)...\n");
        uint32_t waited = 0;
        while (1) {
            thread_sleep(NET_SCAN_POLL_MS);
            waited += NET_SCAN_POLL_MS;
            if (wlan_scan_results(index, r) != SYS_OK) {
                print("the adapter went away\n");
                free(r);
                return 1;
            }
            if (r->state != WLAN_SCAN_RUNNING)
                break;
            if (waited >= NET_SCAN_TIMEOUT_MS) {
                print("still scanning; 'net list' shows what it found so far\n");
                break;
            }
        }
        if (r->state == WLAN_SCAN_FAILED) {
            struct WlanInfo info;
            if (wlan_info(index, &info) == SYS_OK && info.state == WLAN_STATE_RADIO_OFF)
                print("scan failed: the wireless switch has the radio off\n");
            else
                print("scan failed: the radio did not come up (see the log)\n");
            result = 1;
        }
    } else if (strcmp(args[0], "list") == 0) {
        if (wlan_scan_results(index, r) != SYS_OK) {
            print("no such adapter\n");
            free(r);
            return 1;
        }
        if (r->state == WLAN_SCAN_IDLE) {
            print("no scan yet: net scan\n");
            free(r);
            return 0;
        }
        if (r->state == WLAN_SCAN_RUNNING)
            print("(scan still running)\n");
    } else {
        print("usage: net [scan | list] [wlanN]\n");
        free(r);
        return 1;
    }

    if (r->state != WLAN_SCAN_FAILED)
        console_wlan_show_results(index, r);
    free(r);
    return result;
}

int console_cmd_net(int arg_count, char** args) {
    int result = cmd_net_run(arg_count, args);
    console_command_end();
    return result;
}

#endif
