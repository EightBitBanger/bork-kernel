#ifndef _KERNEL_DRIVERS_WLAN_STA_H_
#define _KERNEL_DRIVERS_WLAN_STA_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/arch/x86/drivers/net/wlan.h>
#include <kernel/crypt/aes.h>
#include <kernel/crypt/tkip.h>
#include <kernel/crypt/wpa.h>

//
// A Wi-Fi station: joining one access point and moving data through it,
// with no hardware in it. The driver hands it every frame it receives and
// calls it on a timer; it hands the driver whole 802.11 frames to send
// (already encrypted) and Ethernet frames for the IP stack.
//
// Joining goes:
//
//   JOINING      tuned to the access point's channel, waiting for one of
//                its beacons (its rates and RSN element are needed)
//   AUTH         open system authentication sent, waiting for the answer
//   ASSOC        association request sent (our RSN element in it)
//   HANDSHAKE    WPA2 only: the 4-way handshake (EAPOL-Key messages 1-4)
//                gives the pairwise key (PTK) and the group key (GTK)
//   CONNECTED    data moves; the IP stack gets link (DHCP starts)
//
// Every step has a timeout and a few retries, and a failure says why
// (WLAN_REASON_*). The access point deauthenticating or disassociating us,
// or a long silence (no beacons), ends a connection.
//
// Only what nearly every home network offers: open networks, and
// WPA2-Personal (PSK) with CCMP (AES) for our own (pairwise) traffic. The
// group traffic (broadcasts) may be CCMP or, on a "WPA/WPA2 mixed" network,
// TKIP: that is only ever received, never sent. WPA3 (SAE), TKIP-only, WEP
// and enterprise (802.1X) are refused, with WlanLink.detail saying which.
// Data goes out as plain (non-QoS) data frames, which access points accept
// from stations that don't ask for QoS.
//
// Everything received is untrusted: every length is checked before a byte
// is read; management frames count only from our access point's BSSID;
// EAPOL-Key frames only with a good MIC and a replay counter that went up;
// encrypted data only with a good MIC and a packet number that went up.
//
// No locks or threads here: the driver calls in under its own lock, and
// the ops are called with that lock held.
//

#define WLAN_STA_FRAME_MAX      2400    // Largest 802.11 frame built or taken

// States and reasons: WLAN_LINK_* and WLAN_REASON_* (wlan.h)

struct WlanStaOps {
    // Send one 802.11 frame (no FCS). The hardware adds the FCS and must
    // not encrypt it again. SYS_OK or an error.
    int32_t (*transmit)(void* ctx, const uint8_t* frame, uint32_t length);
    // Tune to `channel` and take frames for `bssid` (and our own address).
    // NULL bssid: back to scanning / idle.
    int32_t (*tune)(void* ctx, uint8_t channel, const uint8_t* bssid);
    // An Ethernet frame for the IP stack (netif_rx)
    void    (*deliver)(void* ctx, const uint8_t* frame, uint32_t length);
    // Link on / off (netif_set_link)
    void    (*link)(void* ctx, bool up);
    // Random bytes (the SNonce)
    void    (*random)(void* ctx, uint8_t* out, uint32_t length);
};

struct WlanSta {
    const struct WlanStaOps* ops;
    void*    ctx;
    uint8_t  mac[6];

    uint8_t  state;             // WLAN_LINK_*
    uint8_t  reason;            // WLAN_REASON_*
    uint8_t  detail;            // WLAN_DETAIL_* with WLAN_REASON_UNSUPPORTED
    uint8_t  retries;
    uint64_t deadline;          // The current step times out then
    uint64_t last_beacon;

    // The network
    struct WlanBss bss;
    bool     secure;            // WPA2-PSK (else open)
    uint8_t  rates[16];         // Supported rates (and extended) from its beacon
    uint8_t  rate_count;
    uint8_t  ap_rsn[64];        // Its RSN element as its beacon had it (checked again in message 3)
    uint8_t  ap_rsn_length;
    bool     group_tkip;        // Its group cipher is TKIP (WPA/WPA2 mixed)
    uint16_t aid;
    uint16_t seq;               // Our next sequence number

    // WPA2
    uint8_t  pmk[WPA_PMK_LEN];
    uint8_t  anonce[WPA_NONCE_LEN];
    uint8_t  snonce[WPA_NONCE_LEN];
    uint64_t replay;            // Last EAPOL-Key replay counter taken from a frame
    bool     replay_valid;      // whose MIC checked out (message 1 has none)
    bool     anonce_set;        // Message 1 seen: anonce and the PTK to check message 3 with
    wpa_ptk_t ptk;
    bool     ptk_set;           // Keys installed: data is encrypted
    aes128_t tk;
    aes128_t gtk[4];
    tkip_key_t gtk_tkip[4];     // The same keys when the group cipher is TKIP
    bool     gtk_set[4];
    uint64_t gtk_rsc[4];        // Last group packet number (CCMP PN / TKIP TSC) per key id
    uint64_t rx_pn[17];         // Last pairwise packet number per TID (16: non-QoS)
    uint64_t tx_pn;

    uint32_t rx_frames, rx_dropped, tx_frames;
    uint8_t  buffer[WLAN_STA_FRAME_MAX];    // Frames built here
    uint8_t  rx_buffer[WLAN_STA_FRAME_MAX]; // Frames received are decrypted here
};

void    wlan_sta_init(struct WlanSta* s, const struct WlanStaOps* ops, void* ctx, const uint8_t mac[6]);

// Join `bss` (from a scan). `pmk` is NULL for an open network. Ends any
// connection there was. SYS_OK, or SYS_EINVAL (a secured network with no
// PMK, or security it can't do: then the link's reason says so).
int32_t wlan_sta_connect(struct WlanSta* s, const struct WlanBss* bss, const uint8_t* pmk, uint64_t now);

// Leave (a deauthentication is sent when there is something to leave)
void    wlan_sta_disconnect(struct WlanSta* s, uint8_t reason);

// Every frame the adapter received (802.11 header first, no FCS)
void    wlan_sta_rx(struct WlanSta* s, const uint8_t* frame, uint32_t length, int8_t rssi, uint64_t now);

// Timeouts and retries; call every 100 ms or so
void    wlan_sta_tick(struct WlanSta* s, uint64_t now);

// An Ethernet frame from the IP stack: sent as a data frame (encrypted
// when keys are set). SYS_OK, SYS_EAGAIN (not connected), or an error.
int32_t wlan_sta_send(struct WlanSta* s, const uint8_t* frame, uint32_t length);

// The state as user space sees it (ip and gateway left 0: the driver fills them)
void    wlan_sta_link(const struct WlanSta* s, struct WlanLink* out);

#endif
