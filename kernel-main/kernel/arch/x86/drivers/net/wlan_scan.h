#ifndef _KERNEL_DRIVERS_WLAN_SCAN_H_
#define _KERNEL_DRIVERS_WLAN_SCAN_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/arch/x86/drivers/net/wlan.h>

//
// Scan helpers with no hardware in them: 802.11 beacons and the results
// table. Everything they read came off the air, so every length is checked
// against what is there before a byte is read. The Wi-Fi card drivers are
// files (sys/drivers/athn-pci, rtw-usb) and reach these through the driver
// table (driver/kapi.h).
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

// A beacon or probe response into `out` (zeroed first). `channel` is used
// when the frame names none. False for anything else, or a frame too
// short to be one.
bool wlan_parse_beacon(const uint8_t* frame, uint32_t length, uint8_t channel, struct WlanBss* out);

// Add or update a network in a table of `max` entries kept strongest
// first. A full table drops its weakest entry for a stronger one.
void wlan_scan_merge(struct WlanBss* table, uint8_t* count, uint8_t max, const struct WlanBss* bss);

#endif
