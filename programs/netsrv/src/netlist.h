#ifndef NETTRAY_NETLIST_H
#define NETTRAY_NETLIST_H

// The networks to show, from a scan's results: one line per name (the
// strongest access point of it), each hidden network on its own line,
// strongest first. Names are bytes off the air from anyone, so the text
// shown is escaped: printable ASCII as is, anything else (and '\') as \xNN.

#include <stdbool.h>
#include <stdint.h>
#include "wlan_abi.h"

#define NETLIST_NAME_MAX    (4 * WLAN_SSID_MAX + 1)     // Every byte as \xNN, and the 0
#define NETLIST_HIDDEN      "(hidden)"

typedef struct {
    char    name[NETLIST_NAME_MAX];     // Escaped, or NETLIST_HIDDEN
    bool    hidden;
    int8_t  rssi;                       // dBm; WLAN_RSSI_UNKNOWN or below: not known
    uint8_t bars;                       // 0 .. 4
    uint8_t channel;
    uint8_t security;                   // WLAN_SEC_*, 0: open
    uint8_t aps;                        // Access points heard with this name
    uint8_t bssid[6];                   // The strongest one's
    uint8_t ssid_length;                // The name as it came (for connecting)
    uint8_t ssid[WLAN_SSID_MAX];
} netlist_entry_t;

typedef struct {
    netlist_entry_t entry[WLAN_SCAN_MAX];
    uint32_t        count;
} netlist_t;

// Fill `out` from `r` (its count is trusted only up to WLAN_SCAN_MAX)
void netlist_build(const struct WlanScanResults* r, netlist_t* out);

// `length` bytes of a network name, escaped into `out` (NETLIST_NAME_MAX)
void netlist_escape(const uint8_t* ssid, uint32_t length, char* out);

// Signal bars for a level in dBm: 4 strong .. 1 weak, 0 not heard / unusable
uint8_t netlist_bars(int8_t rssi);

#endif
