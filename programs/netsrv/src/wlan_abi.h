#ifndef NETTRAY_WLAN_ABI_H
#define NETTRAY_WLAN_ABI_H

// A copy of the kernel's drivers/net/wlan.h ABI (the same one the console's
// net command copies), which must stay the same

#include <stdint.h>

#define WLAN_IOCTL_INFO         0x574C0001u
#define WLAN_IOCTL_SCAN         0x574C0002u
#define WLAN_IOCTL_SCAN_RESULTS 0x574C0003u
#define WLAN_IOCTL_CONNECT      0x574C0004u
#define WLAN_IOCTL_DISCONNECT   0x574C0005u
#define WLAN_IOCTL_LINK         0x574C0006u
#define WLAN_CONNECT_VERSION    1
#define WLAN_LINK_VERSION       1
#define WLAN_PASSPHRASE_MAX     64
#define WLAN_SCAN_VERSION       1
#define WLAN_SCAN_MAX           32
#define WLAN_SSID_MAX           32
#define WLAN_ADAPTER_MAX        10          // wlan0 .. wlan9

#define WLAN_STATE_PROBED       1
#define WLAN_STATE_IDENTIFIED   2
#define WLAN_STATE_NO_EFUSE     3
#define WLAN_STATE_GONE         4
#define WLAN_STATE_RADIO_ON     5
#define WLAN_STATE_RADIO_FAILED 6

#define WLAN_SCAN_IDLE          0
#define WLAN_SCAN_RUNNING       1
#define WLAN_SCAN_DONE          2
#define WLAN_SCAN_FAILED        3

#define WLAN_SEC_WEP            0x01
#define WLAN_SEC_WPA            0x02
#define WLAN_SEC_WPA2           0x04
#define WLAN_SEC_WPA3           0x08
#define WLAN_SEC_ENTERPRISE     0x10

#define WLAN_BSS_HIDDEN         0x01
#define WLAN_BSS_HT             0x02
#define WLAN_BSS_IBSS           0x04

#define WLAN_RSSI_UNKNOWN       (-127)      // At or below: no level heard

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

#define WLAN_LINK_IDLE          0
#define WLAN_LINK_JOINING       1
#define WLAN_LINK_AUTH          2
#define WLAN_LINK_ASSOC         3
#define WLAN_LINK_HANDSHAKE     4
#define WLAN_LINK_CONNECTED     5

#define WLAN_REASON_NONE        0
#define WLAN_REASON_NOT_FOUND   1
#define WLAN_REASON_AUTH        2
#define WLAN_REASON_ASSOC       3
#define WLAN_REASON_PASSWORD    4
#define WLAN_REASON_UNSUPPORTED 5
#define WLAN_REASON_LEFT        6
#define WLAN_REASON_LOST        7
#define WLAN_REASON_USER        8
#define WLAN_REASON_RADIO       9

#define WLAN_DETAIL_NONE        0
#define WLAN_DETAIL_NO_RSN      1
#define WLAN_DETAIL_GROUP       2
#define WLAN_DETAIL_PAIRWISE    3
#define WLAN_DETAIL_AKM         4
#define WLAN_DETAIL_PMF         5
#define WLAN_DETAIL_CHANGED     6
#define WLAN_DETAIL_GTK         7
#define WLAN_DETAIL_OPEN        8

struct WlanConnect {
    uint32_t version;
    uint8_t  bssid[6];
    uint8_t  ssid_length;
    uint8_t  passphrase_length;
    uint8_t  ssid[WLAN_SSID_MAX];
    char     passphrase[WLAN_PASSPHRASE_MAX];
};

struct WlanLink {
    uint32_t version;
    uint8_t  state, reason, channel;
    int8_t   rssi;
    uint8_t  bssid[6];
    uint8_t  ssid_length, security;
    uint8_t  ssid[WLAN_SSID_MAX];
    uint32_t ip, gateway;
    uint8_t  detail;
    uint8_t  reserved8[3];
    uint32_t reserved;
};

_Static_assert(sizeof(struct WlanConnect) == 108 && sizeof(struct WlanLink) == 64, "wlan.h ABI");
_Static_assert(sizeof(struct WlanInfo) == 40 && sizeof(struct WlanBss) == 48 &&
               sizeof(struct WlanScanResults) == 8 + 48 * WLAN_SCAN_MAX, "wlan.h ABI");

#endif
