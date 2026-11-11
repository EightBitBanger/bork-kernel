#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/arch/x86/drivers/net/wlan_sta.h>
#include <kernel/crypt/aes.h>
#include <kernel/crypt/ccmp.h>
#include <kernel/crypt/crypt.h>
#include <kernel/crypt/tkip.h>
#include <kernel/crypt/wpa.h>
#include <kernel/syscall/syscall.h>
#include <kernel/util/string.h>

//
// The station (wlan_sta.h): joining, the WPA2 4-way handshake, and the
// data path between 802.11 frames and Ethernet frames
//

// Step timeouts (ms) and tries
#define JOIN_MS             1500    // For a beacon of it
#define AUTH_MS             300
#define ASSOC_MS            300
#define MGMT_TRIES          4
#define HANDSHAKE_MS        6000    // From association to message 4
#define BEACON_LOST_MS      8000

// Frame control
#define FC0_TYPE_MASK       0x0C
#define FC0_MGMT            0x00
#define FC0_DATA            0x08
#define FC0_QOS             0x80
#define FC0_NODATA          0x40    // Null data subtypes
#define FC0_ASSOC_REQ       0x00
#define FC0_ASSOC_RESP      0x10
#define FC0_REASSOC_RESP    0x30
#define FC0_BEACON          0x80
#define FC0_DISASSOC        0xA0
#define FC0_AUTH            0xB0
#define FC0_DEAUTH          0xC0
#define FC0_SUBTYPE_MASK    0xF0
#define FC1_TODS            0x01
#define FC1_FROMDS          0x02
#define FC1_PROTECTED       0x40
#define FC1_ORDER           0x80

#define HDR_LEN             24
#define LLC_LEN             8
#define ETH_HDR             14
#define ETH_MAX             1514
#define ETHERTYPE_EAPOL     0x888E

#define CAP_ESS             0x0001
#define CAP_PRIVACY         0x0010

#define IE_SSID             0
#define IE_RATES            1
#define IE_RSN              48
#define IE_EXT_RATES        50
#define IE_VENDOR           221

// Reason codes we send
#define REASON_LEAVING      3       // Deauthenticated because sending station is leaving

// EAPOL-Key
#define EAPOL_VERSION       1
#define EAPOL_TYPE_KEY      3
#define KEY_DESC_RSN        2
#define KI_VERSION_MASK     0x0007
#define KI_VERSION_AES      0x0002  // HMAC-SHA1 MIC, AES key wrap
#define KI_PAIRWISE         0x0008
#define KI_INSTALL          0x0040
#define KI_ACK              0x0080
#define KI_MIC              0x0100
#define KI_SECURE           0x0200
#define KI_ERROR            0x0400
#define KI_REQUEST          0x0800
#define KI_ENCRYPTED        0x1000

// Offsets in an EAPOL-Key frame (from the EAPOL header)
#define EK_INFO             5
#define EK_KEYLEN           7
#define EK_REPLAY           9
#define EK_NONCE            17
#define EK_IV               49
#define EK_RSC              65
#define EK_MIC              81
#define EK_DATALEN          97
#define EK_DATA             99

// Our RSN element: CCMP pairwise, PSK, and the network's group cipher
// (CCMP, or TKIP on a WPA/WPA2 mixed network: rsn_element() puts it in)
#define RSN_GROUP_TYPE      7       // Offset of the group cipher's suite type
#define SUITE_TKIP          0x02
#define SUITE_CCMP          0x04
static const uint8_t our_rsn[] = {
    IE_RSN, 20,
    0x01, 0x00,                     // Version 1
    0x00, 0x0F, 0xAC, 0x04,         // Group: CCMP
    0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,     // Pairwise: CCMP
    0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02,     // AKM: PSK
    0x00, 0x00,                     // Capabilities: no PMF
};

// Ours for this network, as association and message 2 must both send it
static uint32_t rsn_element(const struct WlanSta* s, uint8_t* out) {
    memcpy(out, our_rsn, sizeof(our_rsn));
    out[RSN_GROUP_TYPE] = s->group_tkip ? SUITE_TKIP : SUITE_CCMP;
    return sizeof(our_rsn);
}

static const uint8_t llc_snap[6]   = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00 };
static const uint8_t llc_bridge[6] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0xF8 };

static uint16_t get16le(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint16_t get16be(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void put16le(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put16be(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static uint64_t get64be(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static bool is_group(const uint8_t* addr) {
    return (addr[0] & 1) != 0;
}

static bool same_addr(const uint8_t* a, const uint8_t* b) {
    return memcmp(a, b, 6) == 0;
}

//
// State
//

static void clear_keys(struct WlanSta* s) {
    crypt_wipe(s->anonce, sizeof(s->anonce));
    crypt_wipe(s->snonce, sizeof(s->snonce));
    crypt_wipe(&s->ptk, sizeof(s->ptk));
    crypt_wipe(&s->tk, sizeof(s->tk));
    crypt_wipe(s->gtk, sizeof(s->gtk));
    crypt_wipe(s->gtk_tkip, sizeof(s->gtk_tkip));
    memset(s->gtk_set, 0, sizeof(s->gtk_set));
    memset(s->gtk_rsc, 0, sizeof(s->gtk_rsc));
    memset(s->rx_pn, 0, sizeof(s->rx_pn));
    s->ptk_set      = false;
    s->replay_valid = false;
    s->anonce_set   = false;
    s->replay       = 0;
    s->tx_pn        = 0;
}

// Give up on the network: back to idle, saying why
static void fail(struct WlanSta* s, uint8_t reason) {
    bool was_up = s->state == WLAN_LINK_CONNECTED;
    s->state  = WLAN_LINK_IDLE;
    s->reason = reason;
    s->detail = WLAN_DETAIL_NONE;
    clear_keys(s);
    crypt_wipe(s->pmk, sizeof(s->pmk));
    if (was_up && s->ops->link != NULL)
        s->ops->link(s->ctx, false);
    s->ops->tune(s->ctx, 0, NULL);
}

// Give up on security we can't do, saying which part (WLAN_DETAIL_*)
static void refuse(struct WlanSta* s, uint8_t detail) {
    fail(s, WLAN_REASON_UNSUPPORTED);
    s->detail = detail;
}

void wlan_sta_init(struct WlanSta* s, const struct WlanStaOps* ops, void* ctx, const uint8_t mac[6]) {
    memset(s, 0, sizeof(*s));
    s->ops = ops;
    s->ctx = ctx;
    memcpy(s->mac, mac, 6);
    s->state = WLAN_LINK_IDLE;
}

//
// Sending
//

// The 24-byte header of a frame to the access point
static uint32_t header(struct WlanSta* s, uint8_t* p, uint8_t fc0, uint8_t fc1, const uint8_t* a1,
                       const uint8_t* a3) {
    p[0] = fc0;
    p[1] = fc1;
    p[2] = p[3] = 0;                // Duration: the hardware's
    memcpy(p + 4, a1, 6);
    memcpy(p + 10, s->mac, 6);
    memcpy(p + 16, a3, 6);
    put16le(p + 22, (uint16_t)(s->seq << 4));
    s->seq = (uint16_t)((s->seq + 1) & 0x0FFF);
    return HDR_LEN;
}

static bool send(struct WlanSta* s, uint32_t length) {
    if (s->ops->transmit(s->ctx, s->buffer, length) != SYS_OK)
        return false;
    s->tx_frames++;
    return true;
}

static bool send_auth(struct WlanSta* s) {
    uint8_t* p = s->buffer;
    uint32_t n = header(s, p, FC0_AUTH, 0, s->bss.bssid, s->bss.bssid);
    put16le(p + n, 0);              // Open system
    put16le(p + n + 2, 1);          // Transaction 1
    put16le(p + n + 4, 0);          // Status
    return send(s, n + 6);
}

static bool send_assoc(struct WlanSta* s) {
    uint8_t* p = s->buffer;
    uint32_t n = header(s, p, FC0_ASSOC_REQ, 0, s->bss.bssid, s->bss.bssid);
    put16le(p + n, (uint16_t)(CAP_ESS | (s->secure ? CAP_PRIVACY : 0)));
    put16le(p + n + 2, 10);         // Listen interval, in beacons
    n += 4;
    p[n++] = IE_SSID;
    p[n++] = s->bss.ssid_length;
    memcpy(p + n, s->bss.ssid, s->bss.ssid_length);
    n += s->bss.ssid_length;
    // Its own rates back: at most 8 in Supported Rates, the rest Extended
    uint8_t first = s->rate_count > 8 ? 8 : s->rate_count;
    p[n++] = IE_RATES;
    p[n++] = first;
    memcpy(p + n, s->rates, first);
    n += first;
    if (s->rate_count > first) {
        p[n++] = IE_EXT_RATES;
        p[n++] = (uint8_t)(s->rate_count - first);
        memcpy(p + n, s->rates + first, s->rate_count - first);
        n += s->rate_count - first;
    }
    if (s->secure)
        n += rsn_element(s, p + n);
    return send(s, n);
}

static void send_deauth(struct WlanSta* s, uint16_t reason) {
    uint8_t* p = s->buffer;
    uint32_t n = header(s, p, FC0_DEAUTH, 0, s->bss.bssid, s->bss.bssid);
    put16le(p + n, reason);
    send(s, n + 2);
}

// A data frame to the access point, from s->buffer + HDR_LEN + CCMP_HDR_LEN
// where the LLC header and payload (`body` bytes) were put. Encrypted when
// keys are set, unless `clear`.
static bool send_data(struct WlanSta* s, const uint8_t* dst, uint32_t body, bool clear) {
    uint8_t* p = s->buffer;
    bool protect = s->ptk_set && !clear;
    header(s, p, FC0_DATA, (uint8_t)(FC1_TODS | (protect ? FC1_PROTECTED : 0)), s->bss.bssid, dst);
    if (!protect) {
        memmove(p + HDR_LEN, p + HDR_LEN + CCMP_HDR_LEN, body);
        return send(s, HDR_LEN + body);
    }
    if (s->tx_pn >= CCMP_PN_MAX)
        return false;               // Never reuse a packet number: the key must change first
    s->tx_pn++;
    if (!ccmp_encrypt(&s->tk, p, HDR_LEN, s->tx_pn, 0, p + HDR_LEN + CCMP_HDR_LEN, body, p + HDR_LEN))
        return false;
    return send(s, HDR_LEN + CCMP_HDR_LEN + body + CCMP_MIC_LEN);
}

int32_t wlan_sta_send(struct WlanSta* s, const uint8_t* frame, uint32_t length) {
    if (s->state != WLAN_LINK_CONNECTED)
        return SYS_EAGAIN;
    if (length < ETH_HDR || length > ETH_MAX)
        return SYS_EINVAL;
    uint8_t* body = s->buffer + HDR_LEN + CCMP_HDR_LEN;
    memcpy(body, llc_snap, sizeof(llc_snap));
    memcpy(body + 6, frame + 12, 2);                // EtherType
    memcpy(body + LLC_LEN, frame + ETH_HDR, length - ETH_HDR);
    return send_data(s, frame, LLC_LEN + length - ETH_HDR, false) ? SYS_OK : SYS_EIO;
}

//
// The 4-way handshake
//

// An EAPOL-Key frame of ours, after the LLC header: `info` bits, `nonce`
// (NULL: zeros), `data` (key data, NULL: none), MIC'd with the KCK. `clear`:
// unencrypted even once keys are set (message 4 again, for an access point
// that hasn't set its keys yet).
static bool send_key(struct WlanSta* s, uint16_t info, const uint8_t* replay, const uint8_t* nonce,
                     const uint8_t* data, uint32_t data_len, bool clear) {
    uint8_t* body = s->buffer + HDR_LEN + CCMP_HDR_LEN;
    memcpy(body, llc_snap, sizeof(llc_snap));
    put16be(body + 6, ETHERTYPE_EAPOL);
    uint8_t* e = body + LLC_LEN;
    uint32_t total = EK_DATA + data_len;
    memset(e, 0, EK_DATA);
    e[0] = EAPOL_VERSION;
    e[1] = EAPOL_TYPE_KEY;
    put16be(e + 2, (uint16_t)(total - 4));
    e[4] = KEY_DESC_RSN;
    put16be(e + EK_INFO, (uint16_t)(info | KI_VERSION_AES | KI_MIC));
    memcpy(e + EK_REPLAY, replay, 8);
    if (nonce != NULL)
        memcpy(e + EK_NONCE, nonce, WPA_NONCE_LEN);
    put16be(e + EK_DATALEN, (uint16_t)data_len);
    if (data_len > 0)
        memcpy(e + EK_DATA, data, data_len);
    wpa_mic(s->ptk.kck, e, total, e + EK_MIC);
    return send_data(s, s->bss.bssid, LLC_LEN + total, clear);
}

// Find a KDE or element in decrypted key data. GTK KDE: dd len 00-0F-AC-01.
static const uint8_t* find_gtk(const uint8_t* data, uint32_t len, uint32_t* gtk_len, uint8_t* key_id) {
    uint32_t pos = 0;
    while (pos + 2 <= len) {
        uint8_t type = data[pos], n = data[pos + 1];
        if (type == IE_VENDOR && n == 0 && pos + 2 == len)
            break;                  // Padding: dd 00 ...
        if (pos + 2 + n > len)
            break;
        const uint8_t* v = data + pos + 2;
        if (type == IE_VENDOR && n >= 6 + 16 && v[0] == 0x00 && v[1] == 0x0F && v[2] == 0xAC && v[3] == 0x01) {
            *key_id  = v[4] & 3;
            *gtk_len = n - 6;
            return v + 6;
        }
        pos += 2 + n;
    }
    return NULL;
}

static const uint8_t* find_ie(const uint8_t* data, uint32_t len, uint8_t want, uint32_t* ie_len) {
    uint32_t pos = 0;
    while (pos + 2 <= len) {
        uint8_t type = data[pos], n = data[pos + 1];
        if (pos + 2 + n > len)
            return NULL;
        if (type == want) {
            *ie_len = 2 + n;
            return data + pos;
        }
        pos += 2 + n;
    }
    return NULL;
}

static bool install_gtk(struct WlanSta* s, const uint8_t* gtk, uint32_t len, uint8_t key_id, const uint8_t* rsc) {
    if (s->group_tkip) {
        if (len != TKIP_GTK_LEN)
            return false;
        tkip_set_group_key(&s->gtk_tkip[key_id], gtk);
    } else {
        if (len != 16)
            return false;           // A CCMP group key
        aes128_init(&s->gtk[key_id], gtk);
    }
    s->gtk_set[key_id] = true;
    uint64_t pn = 0;
    for (int i = 5; i >= 0; i--)
        pn = (pn << 8) | rsc[i];    // The RSC is little endian; PN in its first 6 bytes
    s->gtk_rsc[key_id] = pn;
    return true;
}

// An EAPOL frame for us (`e`, `len` bytes from the EAPOL header on)
static void handle_eapol(struct WlanSta* s, uint8_t* e, uint32_t len) {
    if (s->state != WLAN_LINK_HANDSHAKE && s->state != WLAN_LINK_CONNECTED)
        return;
    if (!s->secure || len < EK_DATA || e[1] != EAPOL_TYPE_KEY || e[4] != KEY_DESC_RSN)
        return;
    uint32_t body = get16be(e + 2);
    if (body + 4 > len || body + 4 < EK_DATA)
        return;
    len = body + 4;                 // Padding after it is ignored
    uint16_t info = get16be(e + EK_INFO);
    uint32_t data_len = get16be(e + EK_DATALEN);
    if (EK_DATA + data_len > len || (info & KI_VERSION_MASK) != KI_VERSION_AES)
        return;
    if (!(info & KI_ACK) || (info & (KI_REQUEST | KI_ERROR)))
        return;                     // Only the access point's messages
    uint64_t replay = get64be(e + EK_REPLAY);
    if (s->replay_valid && replay <= s->replay)
        return;

    if ((info & KI_PAIRWISE) && !(info & KI_MIC)) {
        // Message 1: its nonce. The PTK follows from it and ours.
        if (s->state != WLAN_LINK_HANDSHAKE)
            return;                 // No rekeying of the PTK: the access point deauthenticates then
        // Message 1 carries no MIC: anyone in range could have sent it. It
        // sets the nonce and the PTK message 3 must prove, but not the
        // replay counter: a forged one with a huge counter would otherwise
        // make us drop the access point's real messages.
        memcpy(s->anonce, e + EK_NONCE, WPA_NONCE_LEN);
        wpa_ptk(s->pmk, s->bss.bssid, s->mac, s->anonce, s->snonce, &s->ptk);
        s->anonce_set = true;
        uint8_t rsn[sizeof(our_rsn)];
        uint32_t rsn_len = rsn_element(s, rsn);
        send_key(s, KI_PAIRWISE, e + EK_REPLAY, s->snonce, rsn, rsn_len, false);
        return;
    }

    // Messages 3 and group key messages carry a MIC
    if (!(info & KI_MIC) || !s->anonce_set)
        return;
    uint8_t mic[WPA_MIC_LEN], want[WPA_MIC_LEN];
    memcpy(want, e + EK_MIC, WPA_MIC_LEN);
    memset(e + EK_MIC, 0, WPA_MIC_LEN);
    wpa_mic(s->ptk.kck, e, len, mic);
    if (!crypt_equal(mic, want, WPA_MIC_LEN))
        return;                     // Not from someone with our PTK

    // Key data, unwrapped with the KEK
    uint8_t plain[256];
    uint32_t plain_len = 0;
    if (info & KI_ENCRYPTED) {
        if (data_len < 24 || data_len % 8 != 0 || data_len - 8 > sizeof(plain))
            return;
        if (!aes_unwrap(s->ptk.kek, e + EK_DATA, data_len, plain))
            return;
        plain_len = data_len - 8;
    }

    if ((info & KI_PAIRWISE) && s->state == WLAN_LINK_CONNECTED) {
        // Message 3 again: our message 4 was lost. Sent again as before,
        // in the clear; the keys stay as they are.
        crypt_wipe(plain, sizeof(plain));
        if ((info & KI_INSTALL) && memcmp(e + EK_NONCE, s->anonce, WPA_NONCE_LEN) == 0) {
            s->replay = replay;
            s->replay_valid = true;
            send_key(s, KI_PAIRWISE | KI_SECURE, e + EK_REPLAY, NULL, NULL, 0, true);
        }
        return;
    }
    if (info & KI_PAIRWISE) {
        // Message 3: the same nonce as message 1, its RSN element as its
        // beacon had it (else something changed it: a downgrade), the GTK
        if (s->state != WLAN_LINK_HANDSHAKE || !(info & KI_INSTALL) || !(info & KI_ENCRYPTED) ||
            memcmp(e + EK_NONCE, s->anonce, WPA_NONCE_LEN) != 0) {
            crypt_wipe(plain, sizeof(plain));
            return;
        }
        uint32_t rsn_len;
        const uint8_t* rsn = find_ie(plain, plain_len, IE_RSN, &rsn_len);
        uint32_t gtk_len;
        uint8_t key_id;
        const uint8_t* gtk = find_gtk(plain, plain_len, &gtk_len, &key_id);
        if (rsn == NULL || rsn_len != s->ap_rsn_length || memcmp(rsn, s->ap_rsn, rsn_len) != 0 || gtk == NULL) {
            crypt_wipe(plain, sizeof(plain));
            refuse(s, gtk == NULL ? WLAN_DETAIL_GTK : WLAN_DETAIL_CHANGED);
            return;
        }
        s->replay = replay;
        s->replay_valid = true;
        if (!send_key(s, KI_PAIRWISE | KI_SECURE, e + EK_REPLAY, NULL, NULL, 0, false)) {
            crypt_wipe(plain, sizeof(plain));
            fail(s, WLAN_REASON_RADIO);
            return;
        }
        // Message 4 went out in the clear; everything after is encrypted
        aes128_init(&s->tk, s->ptk.tk);
        s->ptk_set = true;
        bool ok = install_gtk(s, gtk, gtk_len, key_id, e + EK_RSC);
        crypt_wipe(plain, sizeof(plain));
        if (!ok) {
            refuse(s, WLAN_DETAIL_GTK);
            return;
        }
        s->state       = WLAN_LINK_CONNECTED;
        s->reason      = WLAN_REASON_NONE;
        s->last_beacon = 0;             // Counted from the next tick
        if (s->ops->link != NULL)
            s->ops->link(s->ctx, true);
        return;
    }

    // Group key message 1: a new GTK. Answered with message 2.
    if (s->state != WLAN_LINK_CONNECTED || !(info & KI_ENCRYPTED) || !(info & KI_SECURE)) {
        crypt_wipe(plain, sizeof(plain));
        return;
    }
    uint32_t gtk_len;
    uint8_t key_id;
    const uint8_t* gtk = find_gtk(plain, plain_len, &gtk_len, &key_id);
    if (gtk != NULL && install_gtk(s, gtk, gtk_len, key_id, e + EK_RSC)) {
        s->replay = replay;
        s->replay_valid = true;
        send_key(s, KI_SECURE, e + EK_REPLAY, NULL, NULL, 0, false);
    }
    crypt_wipe(plain, sizeof(plain));
}

//
// Receiving
//

// Its beacon while joining: what it offers, then authentication
static void joined(struct WlanSta* s, const uint8_t* ies, uint32_t len, uint64_t now) {
    s->rate_count = 0;
    uint32_t n;
    const uint8_t* ie = find_ie(ies, len, IE_RATES, &n);
    for (uint32_t i = 2; ie != NULL && i < n && s->rate_count < sizeof(s->rates); i++)
        s->rates[s->rate_count++] = ie[i];
    ie = find_ie(ies, len, IE_EXT_RATES, &n);
    for (uint32_t i = 2; ie != NULL && i < n && s->rate_count < sizeof(s->rates); i++)
        s->rates[s->rate_count++] = ie[i];
    if (s->rate_count == 0) {
        s->rates[0] = 0x82;         // 1 Mbit/s, basic
        s->rate_count = 1;
    }

    const uint8_t* rsn = find_ie(ies, len, IE_RSN, &n);
    if (s->secure) {
        // CCMP pairwise, PSK, no PMF requirement, and a group cipher of
        // CCMP or TKIP (WPA/WPA2 mixed: group traffic is only received)
        if (rsn == NULL || n > sizeof(s->ap_rsn) || n < 2 + 2 + 4 + 2 + 4 + 2 + 4) {
            refuse(s, WLAN_DETAIL_NO_RSN);
            return;
        }
        const uint8_t* v = rsn + 2;
        uint32_t vlen = n - 2, pos = 2;
        bool ok = memcmp(v + pos, our_rsn + 4, 3) == 0 &&               // 00-0F-AC
                  (v[pos + 3] == SUITE_CCMP || v[pos + 3] == SUITE_TKIP);
        s->group_tkip = ok && v[pos + 3] == SUITE_TKIP;
        pos += 4;
        uint32_t count = get16le(v + pos);
        pos += 2;
        bool ccmp = false;
        for (uint32_t i = 0; i < count && pos + 4 <= vlen; i++, pos += 4)
            ccmp |= memcmp(v + pos, our_rsn + 10, 4) == 0;
        bool psk = false;
        if (pos + 2 <= vlen) {
            count = get16le(v + pos);
            pos += 2;
            for (uint32_t i = 0; i < count && pos + 4 <= vlen; i++, pos += 4)
                psk |= memcmp(v + pos, our_rsn + 16, 4) == 0;
        }
        bool pmf_required = pos + 2 <= vlen && (get16le(v + pos) & 0x0040);
        if (!ok || !ccmp || !psk || pmf_required) {
            refuse(s, !ok ? WLAN_DETAIL_GROUP : !ccmp ? WLAN_DETAIL_PAIRWISE :
                      !psk ? WLAN_DETAIL_AKM : WLAN_DETAIL_PMF);
            return;
        }
        memcpy(s->ap_rsn, rsn, n);
        s->ap_rsn_length = (uint8_t)n;
    } else if (rsn != NULL) {
        refuse(s, WLAN_DETAIL_OPEN);        // It wants a password after all
        return;
    }

    s->state    = WLAN_LINK_AUTH;
    s->retries  = 0;
    s->deadline = now + AUTH_MS;
    if (!send_auth(s))
        fail(s, WLAN_REASON_RADIO);
}

static void rx_mgmt(struct WlanSta* s, const uint8_t* f, uint32_t len, int8_t rssi, uint64_t now) {
    if (!same_addr(f + 16, s->bss.bssid) || !same_addr(f + 10, s->bss.bssid))
        return;                     // Not from our access point
    uint8_t subtype = f[0] & FC0_SUBTYPE_MASK;
    const uint8_t* b = f + HDR_LEN;
    uint32_t blen = len - HDR_LEN;

    if (subtype == FC0_BEACON) {
        if (blen < 12)
            return;
        s->bss.rssi = rssi;
        if (s->state == WLAN_LINK_JOINING)
            joined(s, b + 12, blen - 12, now);
        else
            s->last_beacon = now;
        return;
    }
    bool to_us = same_addr(f + 4, s->mac);
    if (!to_us && !(subtype == FC0_DEAUTH || subtype == FC0_DISASSOC))
        return;
    if ((subtype == FC0_DEAUTH || subtype == FC0_DISASSOC) && (to_us || is_group(f + 4))) {
        if (s->state >= WLAN_LINK_ASSOC)
            fail(s, s->state == WLAN_LINK_HANDSHAKE ? WLAN_REASON_PASSWORD : WLAN_REASON_LEFT);
        else if (s->state == WLAN_LINK_AUTH)
            fail(s, WLAN_REASON_AUTH);
        return;
    }
    if (subtype == FC0_AUTH && s->state == WLAN_LINK_AUTH) {
        if (blen < 6 || get16le(b) != 0 || get16le(b + 2) != 2)
            return;
        if (get16le(b + 4) != 0) {
            fail(s, WLAN_REASON_AUTH);
            return;
        }
        s->state    = WLAN_LINK_ASSOC;
        s->retries  = 0;
        s->deadline = now + ASSOC_MS;
        if (!send_assoc(s))
            fail(s, WLAN_REASON_RADIO);
        return;
    }
    if ((subtype == FC0_ASSOC_RESP || subtype == FC0_REASSOC_RESP) && s->state == WLAN_LINK_ASSOC) {
        if (blen < 6)
            return;
        if (get16le(b + 2) != 0) {
            fail(s, WLAN_REASON_ASSOC);
            return;
        }
        s->aid = get16le(b + 4) & 0x3FFF;
        if (!s->secure) {
            s->state       = WLAN_LINK_CONNECTED;
            s->reason      = WLAN_REASON_NONE;
            s->last_beacon = now;
            if (s->ops->link != NULL)
                s->ops->link(s->ctx, true);
            return;
        }
        s->state    = WLAN_LINK_HANDSHAKE;
        s->deadline = now + HANDSHAKE_MS;
        return;
    }
}

static void rx_data(struct WlanSta* s, const uint8_t* f, uint32_t len) {
    uint8_t fc0 = f[0], fc1 = f[1];
    if (fc0 & FC0_NODATA)
        return;                     // Null data: nothing in it
    if ((fc1 & (FC1_TODS | FC1_FROMDS)) != FC1_FROMDS || !same_addr(f + 10, s->bss.bssid))
        return;                     // Only from our access point to the stations
    const uint8_t* dst = f + 4;
    const uint8_t* src = f + 16;
    if (!(same_addr(dst, s->mac) || is_group(dst)) || same_addr(src, s->mac))
        return;                     // Not ours, or our own broadcast coming back
    bool qos = (fc0 & FC0_QOS) != 0;
    uint32_t hdr = HDR_LEN + (qos ? 2 : 0) + ((qos && (fc1 & FC1_ORDER)) ? 4 : 0);
    if (len < hdr || len > sizeof(s->rx_buffer))
        return;
    uint8_t tid = qos ? (f[HDR_LEN] & 0x0F) : 16;
    if (tid > 16)
        tid = 16;

    // Into our buffer: decrypted there when it's protected
    memcpy(s->rx_buffer, f, len);
    uint8_t* body;
    size_t body_len;
    bool protect = (fc1 & FC1_PROTECTED) != 0;
    if (protect && !s->ptk_set) {
        s->rx_dropped++;
        return;
    }
    if (protect) {
        if (len < hdr + CCMP_HDR_LEN + CCMP_MIC_LEN) {
            s->rx_dropped++;
            return;
        }
        uint8_t key_id = (uint8_t)(f[hdr + 3] >> 6);
        bool group = is_group(dst);
        uint64_t pn;
        uint8_t got_id;
        body = s->rx_buffer + hdr + CCMP_HDR_LEN;      // TKIP's IV is as long: TKIP_IV_LEN
        bool ok;
        if (group && s->group_tkip) {
            // WPA/WPA2 mixed: its broadcasts come TKIP encrypted
            ok = s->gtk_set[key_id] &&
                 tkip_decrypt(&s->gtk_tkip[key_id], s->rx_buffer, len, hdr, dst, src,
                              (uint8_t)(qos ? tid : 0), body, &body_len, &pn, &got_id);
        } else {
            const aes128_t* key = group ? (s->gtk_set[key_id] ? &s->gtk[key_id] : NULL) : &s->tk;
            ok = key != NULL && ccmp_decrypt(key, s->rx_buffer, len, hdr, body, &body_len, &pn, &got_id);
        }
        if (!ok) {
            s->rx_dropped++;
            return;
        }
        uint64_t* last = group ? &s->gtk_rsc[key_id] : &s->rx_pn[tid];
        if (pn <= *last) {
            s->rx_dropped++;        // A replay (or a retransmission already taken)
            return;
        }
        *last = pn;
    } else {
        body = s->rx_buffer + hdr;
        body_len = len - hdr;
    }

    if (body_len < LLC_LEN ||
        (memcmp(body, llc_snap, sizeof(llc_snap)) != 0 && memcmp(body, llc_bridge, sizeof(llc_bridge)) != 0)) {
        s->rx_dropped++;
        return;
    }
    uint16_t type = get16be(body + 6);
    if (!protect && s->ptk_set && type != ETHERTYPE_EAPOL) {
        // Once keys are set only EAPOL may come unencrypted (the handshake
        // being repeated); anything else could be anyone's
        s->rx_dropped++;
        return;
    }
    if (type == ETHERTYPE_EAPOL) {
        handle_eapol(s, body + LLC_LEN, (uint32_t)(body_len - LLC_LEN));
        return;
    }
    if (s->state != WLAN_LINK_CONNECTED || body_len - LLC_LEN + ETH_HDR > ETH_MAX)
        return;

    // The Ethernet frame: its header goes over the end of the LLC header
    uint8_t addrs[12];
    memcpy(addrs, dst, 6);
    memcpy(addrs + 6, src, 6);
    uint8_t* eth = body + LLC_LEN - ETH_HDR;
    memcpy(eth, addrs, 12);
    // EtherType is already in place: body + 6, which is eth + 12
    s->rx_frames++;
    s->ops->deliver(s->ctx, eth, (uint32_t)(ETH_HDR + body_len - LLC_LEN));
}

void wlan_sta_rx(struct WlanSta* s, const uint8_t* frame, uint32_t length, int8_t rssi, uint64_t now) {
    if (s->state == WLAN_LINK_IDLE || frame == NULL || length < HDR_LEN || (frame[0] & 0x03) != 0)
        return;
    switch (frame[0] & FC0_TYPE_MASK) {
        case FC0_MGMT: rx_mgmt(s, frame, length, rssi, now); break;
        case FC0_DATA: rx_data(s, frame, length);            break;
        default: break;
    }
}

//
// Joining and leaving
//

int32_t wlan_sta_connect(struct WlanSta* s, const struct WlanBss* bss, const uint8_t* pmk, uint64_t now) {
    if (s->state != WLAN_LINK_IDLE)
        wlan_sta_disconnect(s, WLAN_REASON_USER);
    s->reason = WLAN_REASON_NONE;
    s->detail = WLAN_DETAIL_NONE;
    if (bss->security & (WLAN_SEC_WEP | WLAN_SEC_ENTERPRISE) ||
        (bss->security != 0 && !(bss->security & WLAN_SEC_WPA2))) {
        s->reason = WLAN_REASON_UNSUPPORTED;    // WEP, WPA1 only, WPA3 only, 802.1X
        s->detail = (bss->security & WLAN_SEC_WEP) ? WLAN_DETAIL_GROUP :
                    (bss->security & (WLAN_SEC_ENTERPRISE | WLAN_SEC_WPA3)) ? WLAN_DETAIL_AKM :
                    WLAN_DETAIL_NO_RSN;
        return SYS_EINVAL;
    }
    bool secure = bss->security != 0;
    if (secure && pmk == NULL)
        return SYS_EINVAL;

    clear_keys(s);
    s->bss        = *bss;
    if (s->bss.ssid_length > WLAN_SSID_MAX)
        s->bss.ssid_length = WLAN_SSID_MAX;
    s->secure     = secure;
    s->aid        = 0;
    s->ap_rsn_length = 0;
    s->group_tkip = false;
    if (secure) {
        memcpy(s->pmk, pmk, WPA_PMK_LEN);
        s->ops->random(s->ctx, s->snonce, WPA_NONCE_LEN);
    }
    s->state    = WLAN_LINK_JOINING;
    s->retries  = 0;
    s->deadline = now + JOIN_MS;
    if (s->ops->tune(s->ctx, bss->channel, bss->bssid) != SYS_OK) {
        fail(s, WLAN_REASON_RADIO);
        return SYS_EIO;
    }
    return SYS_OK;
}

void wlan_sta_disconnect(struct WlanSta* s, uint8_t reason) {
    if (s->state == WLAN_LINK_IDLE)
        return;
    if (s->state >= WLAN_LINK_ASSOC)
        send_deauth(s, REASON_LEAVING);
    fail(s, reason);
}

void wlan_sta_tick(struct WlanSta* s, uint64_t now) {
    switch (s->state) {
        case WLAN_LINK_JOINING:
            if (now >= s->deadline)
                fail(s, WLAN_REASON_NOT_FOUND);
            break;
        case WLAN_LINK_AUTH:
        case WLAN_LINK_ASSOC:
            if (now < s->deadline)
                break;
            if (++s->retries >= MGMT_TRIES) {
                fail(s, s->state == WLAN_LINK_AUTH ? WLAN_REASON_AUTH : WLAN_REASON_ASSOC);
                break;
            }
            s->deadline = now + (s->state == WLAN_LINK_AUTH ? AUTH_MS : ASSOC_MS);
            if (!(s->state == WLAN_LINK_AUTH ? send_auth(s) : send_assoc(s)))
                fail(s, WLAN_REASON_RADIO);
            break;
        case WLAN_LINK_HANDSHAKE:
            if (now >= s->deadline) {
                // Message 1 came and our 2 went unanswered: the PMK (the
                // password) didn't match. No message 1 at all: same story
                // from the user's side.
                wlan_sta_disconnect(s, WLAN_REASON_PASSWORD);
            }
            break;
        case WLAN_LINK_CONNECTED:
            if (s->last_beacon == 0 || s->last_beacon > now)
                s->last_beacon = now;
            else if (now - s->last_beacon >= BEACON_LOST_MS)
                wlan_sta_disconnect(s, WLAN_REASON_LOST);
            break;
        default:
            break;
    }
}

void wlan_sta_link(const struct WlanSta* s, struct WlanLink* out) {
    memset(out, 0, sizeof(*out));
    out->version = WLAN_LINK_VERSION;
    out->state   = s->state;
    out->reason  = s->reason;
    out->detail  = s->detail;
    if (s->state != WLAN_LINK_IDLE || s->reason != WLAN_REASON_NONE) {
        out->channel     = s->bss.channel;
        out->rssi        = s->bss.rssi;
        out->security    = s->bss.security;
        out->ssid_length = s->bss.ssid_length;
        memcpy(out->bssid, s->bss.bssid, 6);
        memcpy(out->ssid, s->bss.ssid, sizeof(out->ssid));
    }
}
