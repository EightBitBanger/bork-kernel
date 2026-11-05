#ifndef _KERNEL_DRIVERS_WLAN_H_
#define _KERNEL_DRIVERS_WLAN_H_

#include <stdint.h>

//
// Wireless adapters: /dev/wlan0, /dev/wlan1, ...
//
// Opening one, and every ioctl, needs SYS_SEC_NET (the "network" bit).
// read() returns 0: data goes through the IP stack's interface of the same
// name, not the device. Requests:
//
//   struct WlanInfo info;
//   ioctl(fd, WLAN_IOCTL_INFO, &info);         // the adapter
//
//   ioctl(fd, WLAN_IOCTL_SCAN, 0);             // start a scan (radio on first)
//   struct WlanScanResults r;
//   ioctl(fd, WLAN_IOCTL_SCAN_RESULTS, &r);    // poll until r.state is DONE
//
//   struct WlanConnect c;                      // a network from the last scan
//   ioctl(fd, WLAN_IOCTL_CONNECT, &c);         // starts joining it
//   struct WlanLink l;
//   ioctl(fd, WLAN_IOCTL_LINK, &l);            // poll: JOINING .. CONNECTED, or IDLE and why
//   ioctl(fd, WLAN_IOCTL_DISCONNECT, 0);
//
// Connecting works on open networks and WPA2-Personal (CCMP). The password
// never leaves the kernel's ioctl: it is turned into the key (PBKDF2) there
// and wiped. Once CONNECTED the interface gets link and DHCP addresses it;
// WlanLink.ip says when that happened. Scanning while connected is refused
// (SYS_EBUSY): the radio stays on the network's channel.
//
// A scan is passive: the radio only listens for beacons on channels 1-13
// (2.4 GHz), WLAN_SCAN_DWELL_MS each, and never transmits. It takes a few
// seconds, longer the first time (the radio is brought up then). Networks
// that hide their name show with ssid_length 0.
//
// SSIDs are bytes off the air, from anyone: up to 32 of anything, control
// characters and terminal escapes included. Print them escaped.
//
// SCAN fails with SYS_EBUSY while a scan runs, SYS_EAGAIN while the
// adapter is still being identified (WLAN_STATE_PROBED), SYS_ENOTTY on a
// chip whose radio the kernel can't run yet (the RTL8192EU and AR9485 can),
// SYS_EIO if the radio would not come up.
//

#define WLAN_IOCTL_INFO         0x574C0001u     // 'WL' 1: struct WlanInfo* to fill
#define WLAN_IOCTL_SCAN         0x574C0002u     // 'WL' 2: start a scan (arg unused)
#define WLAN_IOCTL_SCAN_RESULTS 0x574C0003u     // 'WL' 3: struct WlanScanResults* to fill
#define WLAN_IOCTL_CONNECT      0x574C0004u     // 'WL' 4: const struct WlanConnect*
#define WLAN_IOCTL_DISCONNECT   0x574C0005u     // 'WL' 5: arg unused
#define WLAN_IOCTL_LINK         0x574C0006u     // 'WL' 6: struct WlanLink* to fill
#define WLAN_INFO_VERSION   1
#define WLAN_SCAN_VERSION   1
#define WLAN_CONNECT_VERSION 1
#define WLAN_LINK_VERSION   1

// WlanInfo.family
#define WLAN_FAMILY_RTL8821A    1   // RTL8811AU / RTL8821AU (802.11ac 1x1)
#define WLAN_FAMILY_RTL8821C    2   // RTL8811CU / RTL8821CU (802.11ac 1x1)
#define WLAN_FAMILY_RTL8192E    3   // RTL8192EU (802.11n 2x2, 2.4 GHz)
#define WLAN_FAMILY_AR9485      4   // Atheros AR9485 (AR5B125 Mini PCIe, 802.11n 1x1, 2.4 GHz)

// WlanInfo.state
#define WLAN_STATE_PROBED       1   // Bound and identified; reading its efuse
#define WLAN_STATE_IDENTIFIED   2   // Efuse read, MAC address known
#define WLAN_STATE_NO_EFUSE     3   // Efuse would not read: MAC address unknown
#define WLAN_STATE_GONE         4   // Unplugged
#define WLAN_STATE_RADIO_ON     5   // Radio up, receiving
#define WLAN_STATE_RADIO_FAILED 6   // Radio would not come up (see the kernel log)
#define WLAN_STATE_RADIO_OFF    7   // Turned off by the laptop's wireless switch

struct WlanInfo {
    uint32_t version;       // WLAN_INFO_VERSION
    uint16_t vendor;        // USB idVendor / idProduct, or PCI vendor / device ID
    uint16_t product;
    uint8_t  family;        // WLAN_FAMILY_*
    uint8_t  state;         // WLAN_STATE_*
    uint8_t  usb_speed;     // USB_SPEED_* the adapter runs at, 0 on PCI
    uint8_t  mac_valid;     // 1: mac[] holds the adapter's address
    uint8_t  mac[6];
    uint8_t  chip_cut;      // Silicon revision, 0 = A cut (AR9485: MAC revision)
    uint8_t  chip_id;       // RTL8821C family: REG_SYS_CFG2 (0x09 = 8821C). 0 otherwise
    uint32_t sys_cfg;       // REG_SYS_CFG as read at probe (AR9485: AR_SREV)
    char     chip[16];      // "RTL8821CU", NUL terminated
};

_Static_assert(sizeof(struct WlanInfo) == 40, "WlanInfo is ABI");

//
// Scanning
//

#define WLAN_SCAN_MAX           32      // Networks kept per scan (the strongest)
#define WLAN_SSID_MAX           32
#define WLAN_SCAN_DWELL_MS      150     // Listening time per channel
#define WLAN_CHANNEL_MAX        13      // Channels 1-13

// WlanScanResults.state
#define WLAN_SCAN_IDLE          0       // Never scanned
#define WLAN_SCAN_RUNNING       1       // `channel` is the one being listened to
#define WLAN_SCAN_DONE          2
#define WLAN_SCAN_FAILED        3       // The radio would not come up, or the adapter went

// WlanBss.security (bits; 0 = open)
#define WLAN_SEC_WEP            0x01    // Privacy bit with no WPA / RSN element
#define WLAN_SEC_WPA            0x02    // WPA (vendor element)
#define WLAN_SEC_WPA2           0x04    // RSN with PSK or 802.1X
#define WLAN_SEC_WPA3           0x08    // RSN with SAE
#define WLAN_SEC_ENTERPRISE     0x10    // 802.1X key management offered

// WlanBss.flags
#define WLAN_BSS_HIDDEN         0x01    // Empty or all-zero SSID
#define WLAN_BSS_HT             0x02    // 802.11n
#define WLAN_BSS_IBSS           0x04    // Ad hoc, not an access point

// One network heard. 48 bytes.
struct WlanBss {
    uint8_t  bssid[6];
    uint8_t  channel;
    int8_t   rssi;              // dBm, strongest heard
    uint8_t  security;          // WLAN_SEC_*
    uint8_t  flags;             // WLAN_BSS_*
    uint16_t beacon_interval;   // TU (1.024 ms)
    uint8_t  ssid_length;       // 0 .. WLAN_SSID_MAX
    uint8_t  ssid[WLAN_SSID_MAX];   // Raw bytes, not terminated
    uint8_t  reserved[3];
};

_Static_assert(sizeof(struct WlanBss) == 48, "WlanBss is ABI");

struct WlanScanResults {
    uint32_t version;           // WLAN_SCAN_VERSION
    uint8_t  state;             // WLAN_SCAN_*
    uint8_t  channel;           // RUNNING: the channel listened to now
    uint8_t  count;             // Entries of bss[] filled
    uint8_t  firmware;          // 1: the adapter runs its firmware, or needs none
    struct WlanBss bss[WLAN_SCAN_MAX];  // Strongest first
};

_Static_assert(sizeof(struct WlanScanResults) == 8 + 48 * WLAN_SCAN_MAX, "WlanScanResults is ABI");

//
// Connecting
//

#define WLAN_PASSPHRASE_MAX     64      // 8-63 characters, or 64 hex digits (the key itself)

// Which network: its BSSID from the scan results (the kernel takes its
// channel and security from there, not from here), and its name, which is
// needed for a hidden one (and must match otherwise). 108 bytes.
//
// SYS_ENOENT: not in the last scan. SYS_EINVAL: a bad password length or
// character, a name that doesn't match, or security it can't do
// (WlanLink.reason says UNSUPPORTED). SYS_ENOTTY: an adapter that can't
// connect yet.
struct WlanConnect {
    uint32_t version;           // WLAN_CONNECT_VERSION
    uint8_t  bssid[6];
    uint8_t  ssid_length;
    uint8_t  passphrase_length; // 0 for an open network
    uint8_t  ssid[WLAN_SSID_MAX];
    char     passphrase[WLAN_PASSPHRASE_MAX];   // Not terminated
};

_Static_assert(sizeof(struct WlanConnect) == 108, "WlanConnect is ABI");

// WlanLink.state
#define WLAN_LINK_IDLE          0       // Not connected, not trying
#define WLAN_LINK_JOINING       1       // Tuned to it, waiting for its beacon
#define WLAN_LINK_AUTH          2       // Authenticating
#define WLAN_LINK_ASSOC         3       // Associating
#define WLAN_LINK_HANDSHAKE     4       // WPA2 4-way handshake
#define WLAN_LINK_CONNECTED     5

// WlanLink.reason: why the last attempt or connection ended
#define WLAN_REASON_NONE        0
#define WLAN_REASON_NOT_FOUND   1       // No beacon from it on its channel
#define WLAN_REASON_AUTH        2       // Authentication refused or unanswered
#define WLAN_REASON_ASSOC       3       // Association refused or unanswered
#define WLAN_REASON_PASSWORD    4       // The handshake failed: almost always a wrong password
#define WLAN_REASON_UNSUPPORTED 5       // Security this can't do (WPA3 only, TKIP, enterprise, PMF required)
#define WLAN_REASON_LEFT        6       // The access point deauthenticated / disassociated us
#define WLAN_REASON_LOST        7       // Its beacons stopped
#define WLAN_REASON_USER        8       // Disconnected on request
#define WLAN_REASON_RADIO       9       // The adapter failed to send or tune

// The connection. 64 bytes.
struct WlanLink {
    uint32_t version;           // WLAN_LINK_VERSION
    uint8_t  state;             // WLAN_LINK_*
    uint8_t  reason;            // WLAN_REASON_*
    uint8_t  channel;
    int8_t   rssi;              // Of its last beacon
    uint8_t  bssid[6];
    uint8_t  ssid_length;
    uint8_t  security;          // WLAN_SEC_* of the network
    uint8_t  ssid[WLAN_SSID_MAX];
    uint32_t ip;                // From DHCP, host order; 0: none yet
    uint32_t gateway;
    uint32_t reserved[2];
};

_Static_assert(sizeof(struct WlanLink) == 64, "WlanLink is ABI");

//
// Kernel side, for the kernel console (no file handle needed). index is the
// N of wlanN. SYS_OK or the SYS_E* codes above; SYS_ENOENT for no adapter.
//

int32_t wlan_scan_start(uint32_t index);
int32_t wlan_scan_results(uint32_t index, struct WlanScanResults* out);
// WlanInfo of wlan<index>
int32_t wlan_info(uint32_t index, struct WlanInfo* out);

#endif
