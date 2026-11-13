#ifndef RTW_RX_H
#define RTW_RX_H

#include <stdint.h>
#include <stdbool.h>

#include <kernel/arch/x86/drivers/net/wlan_scan.h>

// Walk one USB receive transfer of a Realtek 802.11n/ac chip (24-byte RX
// descriptors, each followed by an optional PHY status and the frame, the
// next one 8-byte aligned). Frames with a CRC or ICV error, chip reports
// (C2H) and anything cut short are skipped. Returns how many frames were
// handed to `fn`. Everything in the buffer came out of the adapter: every
// length is checked against what is there before a byte is read.
uint32_t rtw_rx_walk(const uint8_t* buffer, uint32_t length, bool cck_hipwr, WlanRxFn fn, void* ctx);

#endif
