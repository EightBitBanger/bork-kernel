#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "rtw_rx.h"

//
// Receive buffer
//
// RX descriptor (little endian words), RTL8192E / 8812A / 8821A layout:
//   dw0  bits 0-13 frame length, 14 CRC error, 15 ICV error,
//        16-19 PHY status size in 8-byte units, 24-25 shift, 26 PHY status
//   dw2  bit 28: a chip report (C2H), not a frame
//   dw3  bits 0-6: rate (0-3 are the CCK rates)
//

#define RX_DESC_LEN         24
#define RXDW0_PKTLEN_M      0x00003FFFu
#define RXDW0_CRCERR        0x00004000u
#define RXDW0_ICVERR        0x00008000u
#define RXDW0_INFOSZ_S      16
#define RXDW0_INFOSZ_M      0x0000000Fu
#define RXDW0_SHIFT_S       24
#define RXDW0_SHIFT_M       0x00000003u
#define RXDW0_PHYST         0x04000000u
#define RXDW2_RPT_C2H       0x10000000u
#define RXDW3_RATE_M        0x0000007Fu

// PHY status bytes used (r88e_rx_phystat): 5 agc_rpt, 4 sig_qual
#define PHY_SIG_QUAL        4
#define PHY_AGC_RPT         5

static uint32_t get32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// CCK: from the LNA / VGA gain indexes (FreeBSD rtwn r88e/r92e_get_rssi_cck)
static int8_t rssi_cck(uint8_t agc_rpt, bool hipwr) {
    int lna = (agc_rpt & 0xE0) >> 5;
    int vga = agc_rpt & 0x1F;
    int rssi = 6 - 2 * vga;
    switch (lna) {
        case 7: rssi = vga > 27 ? -100 + 6 : rssi - 100 + 2 * 27; break;
        case 6: rssi += -48 + 2 * 2; break;
        case 5: rssi += -42 + 2 * 7; break;
        case 4: rssi += -36 + 2 * 7; break;
        case 3: rssi += -24 + 2 * 7; break;
        case 2: rssi += -6 + 2 * 5; if (hipwr) rssi -= 6; break;
        case 1: rssi += 8; break;
        default: rssi += 14; break;
    }
    rssi += 10;     // RTL8192E offset
    if (rssi < -100) rssi = -100;
    if (rssi > 0) rssi = 0;
    return (int8_t)rssi;
}

// OFDM: average signal quality
static int8_t rssi_ofdm(uint8_t sig_qual) {
    int rssi = ((sig_qual >> 1) & 0x7F) - 110;
    if (rssi > 0) rssi = 0;
    return (int8_t)rssi;
}

uint32_t rtw_rx_walk(const uint8_t* buffer, uint32_t length, bool cck_hipwr, WlanRxFn fn, void* ctx) {
    uint32_t frames = 0;
    uint32_t pos = 0;

    while (length - pos >= RX_DESC_LEN) {
        const uint8_t* d = buffer + pos;
        uint32_t dw0 = get32le(d);
        uint32_t dw2 = get32le(d + 8);
        uint32_t dw3 = get32le(d + 12);

        uint32_t pktlen = dw0 & RXDW0_PKTLEN_M;
        uint32_t infosz = ((dw0 >> RXDW0_INFOSZ_S) & RXDW0_INFOSZ_M) * 8;
        uint32_t shift  = (dw0 >> RXDW0_SHIFT_S) & RXDW0_SHIFT_M;
        if (pktlen == 0)
            break;

        // All of it must be in this buffer (each term is small: no overflow)
        uint32_t total = RX_DESC_LEN + shift + infosz + pktlen;
        if (total > length - pos)
            break;

        bool skip = (dw0 & (RXDW0_CRCERR | RXDW0_ICVERR)) || (dw2 & RXDW2_RPT_C2H);
        if (!skip && fn != NULL) {
            struct WlanRxFrame f;
            f.data   = d + RX_DESC_LEN + shift + infosz;
            f.length = pktlen;
            f.cck    = (dw3 & RXDW3_RATE_M) < 4;
            f.rssi   = WLAN_RSSI_UNKNOWN;
            if ((dw0 & RXDW0_PHYST) && infosz > PHY_AGC_RPT) {
                const uint8_t* phy = d + RX_DESC_LEN + shift;
                f.rssi = f.cck ? rssi_cck(phy[PHY_AGC_RPT], cck_hipwr) : rssi_ofdm(phy[PHY_SIG_QUAL]);
            }
            fn(ctx, &f);
            frames++;
        }

        // Next descriptor: 8-byte aligned, unless this was the last one
        uint32_t next = (total + 7) & ~7u;
        if (next >= length - pos)
            break;
        pos += next;
    }
    return frames;
}
