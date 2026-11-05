#ifndef _KERNEL_DRIVERS_RTW8192E_H_
#define _KERNEL_DRIVERS_RTW8192E_H_

#include <stdint.h>
#include <stdbool.h>

//
// RTL8192EU radio bring-up, for rtw_usb.c: power on, firmware (optional),
// MAC / DMA / baseband / RF set up for receiving, and tuning to a 2.4 GHz
// channel. Receive only: nothing here sets transmit power or sends.
//
// It reaches the chip through RtwBus, so it has no USB in it (and runs
// against a fake chip in the host tests).
//

struct RtwBus {
    void* ctx;
    // `length` 1, 2 or 4; little endian. False when the transfer failed.
    bool (*read)(void* ctx, uint16_t reg, void* out, uint16_t length);
    // `length` 1 .. RTW_BUS_BLOCK_MAX
    bool (*write)(void* ctx, uint16_t reg, const void* data, uint16_t length);
    void (*sleep_ms)(uint32_t ms);
    uint64_t (*now_ms)(void);
};

#define RTW_BUS_BLOCK_MAX       196     // Largest register write (firmware blocks)

#define R92E_FW_MAX             0x8000  // Firmware image, header included
#define R92E_FW_NAME            "rtl8192eu_nic.bin"

struct R92eRadio {
    struct RtwBus bus;
    bool     failed;            // A register access failed: stop
    bool     fw_loaded;
    bool     cck_hipwr;         // For CCK signal strength
    uint8_t  mac[6];
    uint8_t  crystalcap;        // From the efuse (0x20 when blank)
    uint8_t  channel;
    uint32_t rf_chnlbw[2];
    const char* error;          // What failed, for the log
};

// Bring the radio up to receive on channel 1. `fw` may be NULL (the chip
// then runs without its firmware, which receiving does not need).
// `crystalcap` and `mac` come from the efuse. False with r->error set.
bool r92e_radio_on(struct R92eRadio* r, const uint8_t* fw, uint32_t fw_length);

// Tune to channel 1-13 (20 MHz)
bool r92e_set_channel(struct R92eRadio* r, uint8_t channel);

// Efuse logical map offsets
#define R92E_EFUSE_CRYSTALCAP   0xB9
#define R92E_EFUSE_MAC          0xD7

#endif
