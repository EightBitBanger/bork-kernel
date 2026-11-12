#ifndef _KERNEL_DRIVERS_WLAN_SCAN_H_
#define _KERNEL_DRIVERS_WLAN_SCAN_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/arch/x86/drivers/net/wlan.h>

//
// Scan helpers with no hardware in them: the Realtek USB receive buffer,
// 802.11 beacons, the results table. Everything they read came off the air
// or out of the adapter, so every length is checked against what is there
// before a byte is read.
//

// One received frame, as the adapter's descriptor describes it
struct WlanRxFrame {
    const uint8_t* data;        // The 802.11 frame (header first)
    uint32_t length;
    int8_t   rssi;              // dBm, WLAN_RSSI_UNKNOWN without a PHY status
    bool     cck;               // Received at a CCK rate (1-11 Mbit/s)
};

#define WLAN_RSSI_UNKNOWN       (-127)

typedef void (*WlanRxFn)(void* ctx, const struct WlanRxFrame* frame);

// Walk one USB receive transfer of a Realtek 802.11n/ac chip (24-byte RX
// descriptors, each followed by an optional PHY status and the frame, the
// next one 8-byte aligned). Frames with a CRC or ICV error, chip reports
// (C2H) and anything cut short are skipped. Returns how many frames were
// handed to `fn`.
uint32_t wlan_rx_walk(const uint8_t* buffer, uint32_t length, bool cck_hipwr, WlanRxFn fn, void* ctx);

// A beacon or probe response into `out` (zeroed first). `channel` is used
// when the frame names none. False for anything else, or a frame too
// short to be one.
bool wlan_parse_beacon(const uint8_t* frame, uint32_t length, uint8_t channel, struct WlanBss* out);

// Add or update a network in a table of `max` entries kept strongest
// first. A full table drops its weakest entry for a stronger one.
void wlan_scan_merge(struct WlanBss* table, uint8_t* count, uint8_t max, const struct WlanBss* bss);

#endif
