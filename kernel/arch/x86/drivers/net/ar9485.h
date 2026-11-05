#ifndef _KERNEL_DRIVERS_AR9485_H_
#define _KERNEL_DRIVERS_AR9485_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/arch/x86/drivers/net/wlan_scan.h>

//
// Atheros AR9485 (the AR5B125 Mini PCIe card): the chip, with no PCI and
// no kernel in it, so a host test can run it against a fake chip.
//
// Power on, read the ROM (MAC address, antenna set-up, transmit power),
// tune a 2.4 GHz channel at 20 MHz, hand every frame heard to the caller,
// and send 802.11 frames the caller built (already encrypted: the chip's
// own crypto stays off). The card needs no firmware.
//
// The sequence is OpenBSD's athn(4) (ar9003.c, ar9380.c), ISC licence;
// the PLL set-up and the AR9485 register tables are the same as Linux's
// ath9k, also ISC. See ar9485_tables.h.
//
// Everything the chip returns is untrusted: ROM blocks are bounds checked
// against the image before a byte is copied, a receive buffer is copied
// out of DMA memory and its lengths checked before anything reads the
// frame, and a transmit status only ever completes a frame we queued.
//

#define AR9485_VENDOR           0x168C
#define AR9485_DEVICE           0x0032

// Receive buffers: one page each, the status words first, then the frame
#define AR9485_RXBUF_SIZE       4096
#define AR9485_RX_STATUS_SIZE   48
#define AR9485_RX_DATA_MAX      3824    // What the card may write after the status (athn: 3872 - 48)
#define AR9485_RX_MIN_LEN       (24 + 4)    // An 802.11 header and the FCS
#define AR9485_RX_LP_COUNT      32      // Low-priority FIFO (data, management)
#define AR9485_RX_HP_COUNT      16      // High-priority FIFO (beacons can land here)
#define AR9485_RX_COUNT         (AR9485_RX_LP_COUNT + AR9485_RX_HP_COUNT)

// Transmit buffers: one page each, the 128-byte descriptor first, then the
// frame. As many as the queue's hardware FIFO holds, so a frame is either
// in the card or free. The status ring is one more page.
#define AR9485_TX_COUNT         8
#define AR9485_TX_DESC_SIZE     128
#define AR9485_TX_DATA_MAX      2400    // Largest frame sent (without the FCS)
#define AR9485_TX_STATUS_SIZE   36
#define AR9485_TX_STATUS_COUNT  32
#define AR9485_TX_QUEUE         0       // The one queue used (athn's best effort)

// Transmit rates (the descriptor's rate codes)
#define AR9485_RATE_CCK1        0x1b
#define AR9485_RATE_CCK2        0x1a
#define AR9485_RATE_CCK5        0x19    // 5.5 Mbit/s
#define AR9485_RATE_CCK11       0x18
#define AR9485_RATE_OFDM6       0x0b
#define AR9485_RATE_OFDM9       0x0f
#define AR9485_RATE_OFDM12      0x0a
#define AR9485_RATE_OFDM18      0x0e
#define AR9485_RATE_OFDM24      0x09
#define AR9485_RATE_OFDM36      0x0d
#define AR9485_RATE_OFDM48      0x08
#define AR9485_RATE_OFDM54      0x0c

// Transmit power ceiling, in half dB: the ROM's target powers are used up
// to this (20 dBm, the 2.4 GHz limit nearly everywhere)
#define AR9485_TXPOWER_MAX      40

// The registers, behind the caller's accessors. A dead or removed card
// reads as all ones; nothing here loops on a register without a bound.
struct Ar9485Bus {
    void*    ctx;
    uint32_t (*read)(void* ctx, uint32_t reg);
    void     (*write)(void* ctx, uint32_t reg, uint32_t value);
    void     (*delay_us)(uint32_t us);
};

// One DMA page: its kernel address and the address the card uses
struct Ar9485Page {
    uint8_t* virt;                  // One page (4096 bytes)
    uint32_t phys;
};

// A transmit: up to four rates tried in turn, `tries` times each (0: the
// series ends there)
struct Ar9485TxRates {
    uint8_t rate[4];                // AR9485_RATE_*
    uint8_t tries[4];
};

struct Ar9485 {
    struct Ar9485Bus bus;

    // AR_SREV
    uint32_t srev;
    uint16_t mac_ver;
    uint8_t  mac_rev;
    uint32_t wa;                    // AR_WA, written back before every reset

    // ROM
    bool     rom_ok;
    bool     rom_otp;               // In OTP memory, not an EEPROM
    uint16_t rom_size;              // 4096, 1024 or 512
    uint8_t  rom_blocks;            // Blocks restored
    uint8_t  mac[6];
    bool     mac_valid;
    uint8_t  txmask, rxmask;
    bool     rfsilent;              // A wireless switch is wired to a GPIO
    uint8_t  rfsilent_pin;
    bool     rfsilent_reversed;
    uint8_t* eep;                   // AR9485_EEP_IMAGE_SIZE, NULL until read

    // Receiving. buffers are the caller's; lp_next / hp_next are the ones
    // the card fills next.
    struct Ar9485Page rx[AR9485_RX_COUNT];
    uint8_t  lp_next, hp_next;
    bool     rx_on;
    uint8_t  channel;
    bool     switched_off;          // ar9485_tune found the wireless switch off
    uint8_t  frame[AR9485_RX_DATA_MAX];     // The frame being handed over, off DMA memory

    // Transmitting. Pages are the caller's (all NULL: never transmits).
    // Buffers go out in order: tx_tail is the oldest in the card,
    // tx_queued how many are; status_next is the next status the card
    // writes.
    struct Ar9485Page tx[AR9485_TX_COUNT];
    struct Ar9485Page tx_status;
    bool     tx_on;                 // Tuned with transmit pages: ar9485_tx works
    uint8_t  tx_tail, tx_queued;
    uint8_t  status_next;
    int16_t  txpower[4];            // Half dB, from the ROM: CCK, OFDM 6-24, 36, 54 (for the log)

    // The access point (ar9485_set_bss): frames from it, and to us
    bool     bss_set;
    uint8_t  bssid[6];

    // Counters for the log
    uint32_t rx_frames, rx_errors, rx_bad;
    uint32_t tx_frames, tx_acked, tx_failed, tx_bad;

    const char* error;              // Why the last call failed
};

// The ROM image: struct ar9380_eeprom, 1088 bytes
#define AR9485_EEP_IMAGE_SIZE   1088

// Read AR_SREV. False when the card doesn't answer or is not an AR9485.
bool ar9485_identify(struct Ar9485* chip);

// Reset and wake the chip, set up PCIe power saving and read the ROM into
// `eep` (AR9485_EEP_IMAGE_SIZE bytes, kept by the chip). False when the
// chip won't wake or has no ROM (the MAC address is then unknown).
bool ar9485_attach(struct Ar9485* chip, uint8_t* eep);

// Full reset onto 2.4 GHz channel 1-13, 20 MHz, and start receiving
// beacons into chip->rx (every buffer set). With transmit pages set
// (chip->tx and chip->tx_status), it also sets up transmitting: queue,
// status ring, timing and the ROM's transmit power. Anything still queued
// to send is dropped. Forgets the access point (ar9485_set_bss). False on
// a chip that won't reset or calibrate, or with the laptop's wireless
// switch off (chip->switched_off); chip->error says which.
bool ar9485_tune(struct Ar9485* chip, uint8_t channel);

// After a tune: take frames from this access point and to our address
// (data, management), besides every beacon. The card acknowledges frames
// to us by itself.
void ar9485_set_bss(struct Ar9485* chip, const uint8_t bssid[6]);

// Queue one 802.11 frame (header first, no FCS: the card adds it) at
// `rates`. A frame to a group address is sent once, unacknowledged. The
// card sets the duration field; the sequence number is the caller's.
// False when not tuned to transmit, all buffers are in the card (call
// ar9485_tx_reclaim), or the frame is too long; chip->error says which.
bool ar9485_tx(struct Ar9485* chip, const uint8_t* frame, uint32_t length, const struct Ar9485TxRates* rates);

// Take back the buffers the card has finished sending (acknowledged or
// not: tx_acked / tx_failed count them). Returns how many.
uint32_t ar9485_tx_reclaim(struct Ar9485* chip);

// Hand every frame received so far to `fn` and give the buffers back to
// the card. Returns how many frames were handed over. Frames that failed
// their CRC, short or lying descriptors are counted and dropped.
uint32_t ar9485_rx_harvest(struct Ar9485* chip, WlanRxFn fn, void* ctx);

// Stop receiving and sending (the card stops reading and writing every
// page) and let the chip sleep. Safe to call on a chip that isn't
// receiving.
void ar9485_stop(struct Ar9485* chip);

#endif
