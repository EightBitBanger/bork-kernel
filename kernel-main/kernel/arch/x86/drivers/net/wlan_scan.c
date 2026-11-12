#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/arch/x86/drivers/net/wlan_scan.h>
#include <kernel/util/string.h>

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

static uint16_t get16le(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
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

uint32_t wlan_rx_walk(const uint8_t* buffer, uint32_t length, bool cck_hipwr, WlanRxFn fn, void* ctx) {
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

//
// 802.11 management frames
//

#define WH_LEN              24      // Frame control .. sequence control
#define BEACON_FIXED        12      // Timestamp, interval, capabilities
#define FC0_TYPE_MASK       0x0C
#define FC0_TYPE_MGT        0x00
#define FC0_SUBTYPE_MASK    0xF0
#define FC0_BEACON          0x80
#define FC0_PROBE_RESP      0x50
#define FC0_VERSION_MASK    0x03

#define CAP_ESS             0x0001
#define CAP_IBSS            0x0002
#define CAP_PRIVACY         0x0010

#define IE_SSID             0
#define IE_DS_PARAMS        3
#define IE_HT_CAPS          45
#define IE_RSN              48
#define IE_HT_OP            61
#define IE_VENDOR           221

// RSN suites are 00-0F-AC:<type>; AKM types
#define AKM_8021X           1
#define AKM_PSK             2
#define AKM_8021X_SHA256    5
#define AKM_PSK_SHA256      6
#define AKM_SAE             8
#define AKM_8021X_SUITE_B   12
#define AKM_8021X_SUITE_B192 13
#define AKM_FT_SAE          9

static bool oui_is(const uint8_t* p, uint8_t a, uint8_t b, uint8_t c) {
    return p[0] == a && p[1] == b && p[2] == c;
}

// The key management offered by an RSN element (security bits). Every
// count is checked against the element's own length.
static uint8_t parse_rsn(const uint8_t* ie, uint32_t len) {
    // version (2), group cipher (4), pairwise count (2) + 4 each, AKM count (2) + 4 each
    if (len < 2 + 4 + 2)
        return WLAN_SEC_WPA2;       // Truncated after the version: still RSN
    uint32_t pos = 2 + 4;
    uint32_t pairwise = get16le(ie + pos);
    pos += 2;
    if (pairwise > (len - pos) / 4)
        return WLAN_SEC_WPA2;
    pos += pairwise * 4;
    if (len - pos < 2)
        return WLAN_SEC_WPA2;
    uint32_t akms = get16le(ie + pos);
    pos += 2;
    if (akms > (len - pos) / 4)
        return WLAN_SEC_WPA2;

    uint8_t sec = 0;
    for (uint32_t i = 0; i < akms; i++) {
        const uint8_t* s = ie + pos + i * 4;
        if (!oui_is(s, 0x00, 0x0F, 0xAC))
            continue;
        switch (s[3]) {
            case AKM_SAE: case AKM_FT_SAE:
                sec |= WLAN_SEC_WPA3; break;
            case AKM_8021X: case AKM_8021X_SHA256: case AKM_8021X_SUITE_B: case AKM_8021X_SUITE_B192:
                sec |= WLAN_SEC_WPA2 | WLAN_SEC_ENTERPRISE; break;
            default:
                sec |= WLAN_SEC_WPA2; break;
        }
    }
    return sec != 0 ? sec : WLAN_SEC_WPA2;
}

bool wlan_parse_beacon(const uint8_t* frame, uint32_t length, uint8_t channel, struct WlanBss* out) {
    memset(out, 0, sizeof(*out));
    if (frame == NULL || length < WH_LEN + BEACON_FIXED)
        return false;
    uint8_t fc0 = frame[0];
    if ((fc0 & FC0_VERSION_MASK) != 0 || (fc0 & FC0_TYPE_MASK) != FC0_TYPE_MGT)
        return false;
    uint8_t subtype = fc0 & FC0_SUBTYPE_MASK;
    if (subtype != FC0_BEACON && subtype != FC0_PROBE_RESP)
        return false;

    // addr3 is the BSSID; a group address there is not a network
    const uint8_t* bssid = frame + 16;
    if (bssid[0] & 0x01)
        return false;
    memcpy(out->bssid, bssid, 6);

    const uint8_t* body = frame + WH_LEN;
    out->beacon_interval = get16le(body + 8);
    uint16_t caps = get16le(body + 10);
    if ((caps & CAP_IBSS) && !(caps & CAP_ESS))
        out->flags |= WLAN_BSS_IBSS;

    uint8_t sec = 0;
    bool privacy = (caps & CAP_PRIVACY) != 0;
    bool have_ssid = false;
    uint8_t ds_channel = 0, ht_channel = 0;

    // Elements: id, length, data. Stop at the first that runs past the end.
    uint32_t pos = WH_LEN + BEACON_FIXED;
    while (length - pos >= 2) {
        uint8_t id = frame[pos];
        uint32_t len = frame[pos + 1];
        if (len > length - pos - 2)
            break;
        const uint8_t* ie = frame + pos + 2;

        switch (id) {
            case IE_SSID:
                if (!have_ssid && len <= WLAN_SSID_MAX) {
                    memcpy(out->ssid, ie, len);
                    out->ssid_length = (uint8_t)len;
                    have_ssid = true;
                }
                break;
            case IE_DS_PARAMS:
                if (len >= 1)
                    ds_channel = ie[0];
                break;
            case IE_HT_OP:
                if (len >= 1)
                    ht_channel = ie[0];
                out->flags |= WLAN_BSS_HT;
                break;
            case IE_HT_CAPS:
                out->flags |= WLAN_BSS_HT;
                break;
            case IE_RSN:
                sec |= parse_rsn(ie, len);
                break;
            case IE_VENDOR:
                // WPA: 00-50-F2 type 1
                if (len >= 4 && oui_is(ie, 0x00, 0x50, 0xF2) && ie[3] == 1)
                    sec |= WLAN_SEC_WPA;
                break;
            default:
                break;
        }
        pos += 2 + len;
    }

    if (privacy && sec == 0)
        sec = WLAN_SEC_WEP;
    out->security = sec;

    // An empty name, or one of zero bytes, hides the network
    bool hidden = out->ssid_length == 0;
    if (!hidden) {
        hidden = true;
        for (uint32_t i = 0; i < out->ssid_length; i++)
            if (out->ssid[i] != 0)
                hidden = false;
    }
    if (hidden) {
        out->flags |= WLAN_BSS_HIDDEN;
        out->ssid_length = 0;
        memset(out->ssid, 0, sizeof(out->ssid));
    }

    // The frame's own word for its channel, if it is a 2.4 GHz one
    uint8_t ch = ds_channel != 0 ? ds_channel : ht_channel;
    out->channel = (ch >= 1 && ch <= 14) ? ch : channel;
    out->rssi = WLAN_RSSI_UNKNOWN;
    return true;
}

//
// Results table
//

void wlan_scan_merge(struct WlanBss* table, uint8_t* count, uint8_t max, const struct WlanBss* bss) {
    if (*count > max)
        *count = max;

    int found = -1;
    for (uint32_t i = 0; i < *count && found < 0; i++)
        if (memcmp(table[i].bssid, bss->bssid, 6) == 0)
            found = (int)i;

    struct WlanBss entry = *bss;
    if (found >= 0) {
        // Keep the strongest reading, and a name if one was ever heard
        if (table[found].rssi > entry.rssi)
            entry.rssi = table[found].rssi;
        if (entry.ssid_length == 0 && table[found].ssid_length != 0) {
            memcpy(entry.ssid, table[found].ssid, sizeof(entry.ssid));
            entry.ssid_length = table[found].ssid_length;
            entry.flags &= (uint8_t)~WLAN_BSS_HIDDEN;
        }
        // Take it out; it goes back in at its place below
        for (uint32_t i = (uint32_t)found; i + 1 < *count; i++)
            table[i] = table[i + 1];
        (*count)--;
    } else if (*count == max) {
        if (max == 0 || table[max - 1].rssi >= entry.rssi)
            return;     // Weaker than everything kept
        (*count)--;     // Drop the weakest
    }

    // Insert, strongest first
    uint32_t at = *count;
    while (at > 0 && table[at - 1].rssi < entry.rssi) {
        table[at] = table[at - 1];
        at--;
    }
    table[at] = entry;
    (*count)++;
}
