// Host test for the Wi-Fi station (kernel/arch/x86/drivers/net/wlan_sta.c)
// against a simulated access point: open and WPA2-PSK networks, a wrong
// password, the data path both ways, replays and tampering, group rekeying,
// a lost message 4, deauthentication, beacon loss, and networks it must
// refuse.
//
// The access point here is written from the standard, separately from the
// station, using kernel/crypt for its keys (whose test vectors are in
// tests/crypt).
//
//   make            (builds and runs it)

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kernel/arch/x86/drivers/net/wlan_sta.h>
#include <kernel/crypt/aes.h>
#include <kernel/crypt/ccmp.h>
#include <kernel/crypt/random.h>
#include <kernel/crypt/sha1.h>
#include <kernel/crypt/wpa.h>
#include <kernel/syscall/syscall.h>

static int checks, failures;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void put16le(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put16be(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get16le(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint16_t get16be(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

static const uint8_t STA_MAC[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
static const uint8_t AP_MAC[6]  = { 0x0A, 0xBB, 0xCC, 0xDD, 0xEE, 0x01 };
static const uint8_t ROUTER[6]  = { 0x0A, 0xBB, 0xCC, 0xDD, 0xEE, 0x99 };     // Behind the AP
static const uint8_t BCAST[6]   = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

//
// The access point
//

typedef struct {
    // Configuration
    char     ssid[33];
    char     passphrase[64];
    bool     open;
    uint8_t  group_cipher;          // 4: CCMP, 2: TKIP
    uint8_t  pairwise_cipher;       // 4: CCMP, 2: TKIP
    bool     pmf_required;
    bool     on_air;                // Sends beacons when the station tunes in

    // State
    uint8_t  pmk[32];
    uint8_t  anonce[32];
    uint64_t replay;
    wpa_ptk_t ptk;
    bool     m2_ok, keys;
    aes128_t tk, gtk;
    uint8_t  gtk_raw[32];           // CCMP: 16 bytes; TKIP: 32 (TK, Tx MIC, Rx MIC)
    uint8_t  gtk_id;
    uint64_t tx_pn, gtk_pn, rx_pn;
    uint16_t seq;
    int      m2_count, m4_count, group2_count;
    bool     drop_next_m4;
    bool     m3_bad_rsn;
    bool     silent;                // Ignores message 2
    uint8_t  last_deauth;           // Reason the station sent, 0: none

    // What the station sent on, decrypted
    uint8_t  got[2000];
    uint32_t got_len;
    uint8_t  got_dst[6];
    int      got_count;
} ap_t;

static ap_t ap;

// Frames to the station wait here (no recursion into wlan_sta_rx)
#define QUEUE_MAX 16
static struct { uint8_t data[2400]; uint32_t len; } queue[QUEUE_MAX];
static int queued;

static struct WlanSta sta;
static uint64_t now_ms = 1000;
static bool link_up;
static int  link_changes;
static uint8_t tuned_channel;
static bool tune_fail;

// Delivered Ethernet frames
static uint8_t delivered[2000];
static uint32_t delivered_len;
static int delivered_count;

static void to_sta(const uint8_t* f, uint32_t len) {
    if (queued < QUEUE_MAX) {
        memcpy(queue[queued].data, f, len);
        queue[queued].len = len;
        queued++;
    }
}

static uint32_t ap_header(uint8_t* p, uint8_t fc0, uint8_t fc1, const uint8_t* a1, const uint8_t* a3) {
    p[0] = fc0; p[1] = fc1; p[2] = p[3] = 0;
    memcpy(p + 4, a1, 6);
    memcpy(p + 10, AP_MAC, 6);
    memcpy(p + 16, a3, 6);
    put16le(p + 22, (uint16_t)(ap.seq++ << 4));
    return 24;
}

static uint32_t rsn_ie(uint8_t* p) {
    uint8_t ie[] = { 48, 20, 1, 0, 0x00, 0x0F, 0xAC, ap.group_cipher, 1, 0, 0x00, 0x0F, 0xAC, ap.pairwise_cipher,
                     1, 0, 0x00, 0x0F, 0xAC, 2, (uint8_t)(ap.pmf_required ? 0xC0 : 0x00), 0 };
    memcpy(p, ie, sizeof(ie));
    return sizeof(ie);
}

static void ap_beacon(void) {
    uint8_t f[256];
    uint32_t n = ap_header(f, 0x80, 0, BCAST, AP_MAC);
    memset(f + n, 0, 8); n += 8;                    // Timestamp
    put16le(f + n, 100); n += 2;
    put16le(f + n, (uint16_t)(0x0001 | (ap.open ? 0 : 0x0010))); n += 2;
    f[n++] = 0; f[n++] = (uint8_t)strlen(ap.ssid);
    memcpy(f + n, ap.ssid, strlen(ap.ssid)); n += (uint32_t)strlen(ap.ssid);
    uint8_t rates[] = { 1, 8, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24, 50, 4, 0x30, 0x48, 0x60, 0x6C,
                        3, 1, 6 };
    memcpy(f + n, rates, sizeof(rates)); n += sizeof(rates);
    if (!ap.open)
        n += rsn_ie(f + n);
    to_sta(f, n);
}

// AES key wrap (RFC 3394), for the AP's side
static void aes_wrap(const uint8_t kek[16], const uint8_t* in, size_t len, uint8_t* out) {
    aes128_t a;
    aes128_init(&a, kek);
    size_t n = len / 8;
    uint8_t A[8], b[16];
    memset(A, 0xA6, 8);
    memcpy(out + 8, in, len);
    for (int j = 0; j <= 5; j++)
        for (size_t i = 1; i <= n; i++) {
            memcpy(b, A, 8);
            memcpy(b + 8, out + 8 * i, 8);
            aes128_encrypt(&a, b, b);
            uint64_t t = (uint64_t)n * j + i;
            memcpy(A, b, 8);
            for (int k = 0; k < 8; k++)
                A[7 - k] ^= (uint8_t)(t >> (8 * k));
            memcpy(out + 8 * i, b + 8, 8);
        }
    memcpy(out, A, 8);
}

// A data frame from the AP to the station (or everyone): LLC + payload,
// encrypted when keys are set (group frames with the GTK)
static void ap_data(const uint8_t* dst, const uint8_t* src, uint16_t type, const uint8_t* payload, uint32_t len,
                    bool protect, bool qos) {
    uint8_t f[2400];
    uint32_t n = ap_header(f, qos ? 0x88 : 0x08, (uint8_t)(0x02 | (protect ? 0x40 : 0)), dst, src);
    if (qos) { f[n++] = 3; f[n++] = 0; }            // TID 3
    uint8_t body[2000];
    uint8_t llc[8] = { 0xAA, 0xAA, 0x03, 0, 0, 0, 0, 0 };
    put16be(llc + 6, type);
    memcpy(body, llc, 8);
    memcpy(body + 8, payload, len);
    if (!protect) {
        memcpy(f + n, body, 8 + len);
        to_sta(f, n + 8 + len);
        return;
    }
    bool group = dst[0] & 1;
    if (group && ap.group_cipher == 2) {
        // TKIP, as a WPA/WPA2 mixed access point sends its broadcasts:
        // IV, then RC4 over MSDU + Michael MIC + ICV
        uint64_t tsc = ++ap.gtk_pn;
        uint8_t* iv = f + n;
        iv[0] = (uint8_t)(tsc >> 8);
        iv[1] = (uint8_t)((iv[0] | 0x20) & 0x7F);
        iv[2] = (uint8_t)tsc;
        iv[3] = (uint8_t)(0x20 | (ap.gtk_id << 6));
        for (int i = 0; i < 4; i++)
            iv[4 + i] = (uint8_t)(tsc >> (16 + 8 * i));
        uint8_t mh[16 + 2000];
        memset(mh, 0, 16);
        memcpy(mh, dst, 6);
        memcpy(mh + 6, src, 6);
        mh[12] = qos ? 3 : 0;
        memcpy(mh + 16, body, 8 + len);
        uint32_t m = 8 + len;
        tkip_michael(ap.gtk_raw + 16, mh, 16 + m, body + m);
        uint32_t icv = tkip_crc32(body, m + 8);
        for (int i = 0; i < 4; i++)
            body[m + 8 + i] = (uint8_t)(icv >> (8 * i));
        uint8_t key[16];
        tkip_mix(ap.gtk_raw, AP_MAC, tsc, key);
        tkip_rc4(key, body, m + 12, f + n + 8);
        to_sta(f, n + 8 + m + 12);
        return;
    }
    uint64_t pn = group ? ++ap.gtk_pn : ++ap.tx_pn;
    ccmp_encrypt(group ? &ap.gtk : &ap.tk, f, n, pn, group ? ap.gtk_id : 0, body, 8 + len, f + n);
    to_sta(f, n + 8 + 8 + len + 8);
}

// An EAPOL-Key frame from the AP
static void ap_key(uint16_t info, const uint8_t* nonce, const uint8_t* data, uint32_t data_len, bool protect) {
    uint8_t e[512];
    memset(e, 0, 99);
    e[0] = 2; e[1] = 3;
    put16be(e + 2, (uint16_t)(95 + data_len));
    e[4] = 2;
    put16be(e + 5, (uint16_t)(info | 2));
    put16be(e + 7, 16);
    ap.replay++;
    for (int i = 0; i < 8; i++)
        e[9 + i] = (uint8_t)(ap.replay >> (56 - 8 * i));
    if (nonce) memcpy(e + 17, nonce, 32);
    put16be(e + 97, (uint16_t)data_len);
    if (data_len > 0)
        memcpy(e + 99, data, data_len);
    if (info & 0x0100)
        wpa_mic(ap.ptk.kck, e, 99 + data_len, e + 81);
    ap_data(STA_MAC, AP_MAC, 0x888E, e, 99 + data_len, protect, false);
}

static void ap_m1(void) {
    ap_key(0x0008 | 0x0080, ap.anonce, NULL, 0, false);
}

// Key data for M3 / group message 1: [RSN] GTK KDE, padded, wrapped
static uint32_t gtk_data(uint8_t* out, bool with_rsn) {
    uint8_t plain[128];
    uint32_t n = 0;
    if (with_rsn) {
        n += rsn_ie(plain);
        if (ap.m3_bad_rsn)
            plain[7] = 2;           // Says TKIP group: not what the beacon said
    }
    uint8_t glen = ap.group_cipher == 2 ? 32 : 16;
    uint8_t kde[] = { 0xDD, (uint8_t)(6 + glen), 0x00, 0x0F, 0xAC, 0x01, ap.gtk_id, 0 };
    memcpy(plain + n, kde, 8); n += 8;
    memcpy(plain + n, ap.gtk_raw, glen); n += glen;
    if (n % 8) { plain[n++] = 0xDD; while (n % 8) plain[n++] = 0; }
    aes_wrap(ap.ptk.kek, plain, n, out);
    return n + 8;
}

static void ap_m3(void) {
    uint8_t data[160];
    uint32_t n = gtk_data(data, true);
    ap_key(0x0008 | 0x0040 | 0x0080 | 0x0100 | 0x0200 | 0x1000, ap.anonce, data, n, false);
}

static void ap_set_gtk(uint8_t id, uint8_t fill) {
    ap.gtk_id = id;
    for (int i = 0; i < 32; i++)
        ap.gtk_raw[i] = (uint8_t)(fill + i * 7);
    aes128_init(&ap.gtk, ap.gtk_raw);
    ap.gtk_pn = 0;
}

static bool mic_ok(uint8_t* e, uint32_t len) {
    uint8_t want[16], got[16];
    memcpy(want, e + 81, 16);
    memset(e + 81, 0, 16);
    wpa_mic(ap.ptk.kck, e, len, got);
    memcpy(e + 81, want, 16);
    return memcmp(want, got, 16) == 0;
}

// What the station sent
static void ap_eapol(uint8_t* e, uint32_t len) {
    uint16_t info = get16be(e + 5);
    if ((info & 0x0008) && !(info & 0x0200)) {
        // M2: the SNonce gives the PTK; the MIC says whether the PMK matched
        ap.m2_count++;
        if (ap.silent)
            return;
        uint8_t snonce[32];
        memcpy(snonce, e + 17, 32);
        wpa_ptk(ap.pmk, AP_MAC, STA_MAC, ap.anonce, snonce, &ap.ptk);
        if (!mic_ok(e, len)) {
            if (ap.m2_count < 4) {
                ap_m1();
            } else {
                uint8_t f[64];
                uint32_t n = ap_header(f, 0xC0, 0, STA_MAC, AP_MAC);
                put16le(f + n, 15);                 // 4-way handshake timeout
                to_sta(f, n + 2);
            }
            return;
        }
        // Its RSN element must be in the key data
        CHECK(get16be(e + 97) == 22 && e[99] == 48 && e[99 + 7] == ap.group_cipher);
        ap.m2_ok = true;
        ap_m3();
        return;
    }
    if ((info & 0x0008) && (info & 0x0200)) {
        ap.m4_count++;
        CHECK(mic_ok(e, len));
        if (ap.drop_next_m4) {
            ap.drop_next_m4 = false;
            ap_m3();                // As if it never came
            return;
        }
        ap.keys = true;
        aes128_init(&ap.tk, ap.ptk.tk);
        return;
    }
    if (!(info & 0x0008) && (info & 0x0200)) {
        ap.group2_count++;
        CHECK(mic_ok(e, len));
    }
}

// The station's transmit op: the AP takes the frame
static int32_t sta_transmit(void* ctx, const uint8_t* f, uint32_t len) {
    (void)ctx;
    CHECK(len >= 24 && memcmp(f + 10, STA_MAC, 6) == 0 && memcmp(f + 4, AP_MAC, 6) == 0);
    uint8_t fc0 = f[0], fc1 = f[1];
    const uint8_t* b = f + 24;
    if (fc0 == 0xB0) {                              // Auth
        CHECK(get16le(b) == 0 && get16le(b + 2) == 1);
        uint8_t r[64];
        uint32_t n = ap_header(r, 0xB0, 0, STA_MAC, AP_MAC);
        put16le(r + n, 0); put16le(r + n + 2, 2); put16le(r + n + 4, 0);
        to_sta(r, n + 6);
        return SYS_OK;
    }
    if (fc0 == 0x00) {                              // Assoc request
        // SSID, rates, RSN as offered
        CHECK(b[4] == 0 && b[5] == strlen(ap.ssid) && memcmp(b + 6, ap.ssid, b[5]) == 0);
        const uint8_t* ie = b + 6 + b[5];
        CHECK(ie[0] == 1 && ie[1] == 8 && ie[2] == 0x82);
        ie += 2 + ie[1];
        CHECK(ie[0] == 50 && ie[1] == 4);
        ie += 2 + ie[1];
        if (!ap.open)
            CHECK(ie[0] == 48 && ie[1] == 20 && ie[7] == ap.group_cipher && ie[13] == 4 && ie[19] == 2);
        uint8_t r[64];
        uint32_t n = ap_header(r, 0x10, 0, STA_MAC, AP_MAC);
        put16le(r + n, 0x0011); put16le(r + n + 2, 0); put16le(r + n + 4, 0xC001);
        to_sta(r, n + 6);
        if (!ap.open) {
            crypt_random_bytes(ap.anonce, 32);
            ap_m1();
        }
        return SYS_OK;
    }
    if (fc0 == 0xC0) {
        ap.last_deauth = b[0];
        ap.keys = false;
        return SYS_OK;
    }
    if (fc0 == 0x08) {
        CHECK((fc1 & 0x03) == 0x01);                // To DS
        uint8_t body[2000];
        size_t blen;
        if (fc1 & 0x40) {
            uint64_t pn;
            uint8_t id;
            CHECK(ap.keys);
            bool ok = ccmp_decrypt(&ap.tk, f, len, 24, body, &blen, &pn, &id);
            CHECK(ok && pn > ap.rx_pn);
            if (!ok)
                return SYS_OK;
            ap.rx_pn = pn;
        } else {
            blen = len - 24;
            memcpy(body, b, blen);
        }
        CHECK(blen >= 8 && body[0] == 0xAA && body[1] == 0xAA && body[2] == 0x03);
        uint16_t type = get16be(body + 6);
        if (type == 0x888E) {
            ap_eapol(body + 8, (uint32_t)(blen - 8));
            return SYS_OK;
        }
        CHECK(ap.open || (fc1 & 0x40));             // Data is never sent in the clear on WPA2
        memcpy(ap.got, body + 8, blen - 8);
        ap.got_len = (uint32_t)(blen - 8);
        memcpy(ap.got_dst, f + 16, 6);
        ap.got_count++;
        return SYS_OK;
    }
    return SYS_OK;
}

static int32_t sta_tune(void* ctx, uint8_t channel, const uint8_t* bssid) {
    (void)ctx;
    if (tune_fail)
        return SYS_EIO;
    tuned_channel = channel;
    if (bssid != NULL && ap.on_air)
        ap_beacon();
    return SYS_OK;
}

static void sta_deliver(void* ctx, const uint8_t* f, uint32_t len) {
    (void)ctx;
    memcpy(delivered, f, len);
    delivered_len = len;
    delivered_count++;
}

static void sta_link(void* ctx, bool up) {
    (void)ctx;
    link_up = up;
    link_changes++;
}

static void sta_random(void* ctx, uint8_t* out, uint32_t len) {
    (void)ctx;
    crypt_random_bytes(out, len);
}

static const struct WlanStaOps ops = { sta_transmit, sta_tune, sta_deliver, sta_link, sta_random };

//
// Driving it
//

// Hand queued frames over and tick, for `ms`
static void run(uint32_t ms) {
    uint64_t until = now_ms + ms;
    do {
        while (queued > 0) {
            uint8_t f[2400];
            uint32_t len = queue[0].len;
            memcpy(f, queue[0].data, len);
            memmove(queue, queue + 1, sizeof(queue[0]) * (size_t)(queued - 1));
            queued--;
            wlan_sta_rx(&sta, f, len, -55, now_ms);
        }
        wlan_sta_tick(&sta, now_ms);
        now_ms += 50;
    } while (now_ms < until);
}

static void ap_reset(const char* ssid, const char* pass, bool open) {
    memset(&ap, 0, sizeof(ap));
    strcpy(ap.ssid, ssid);
    strcpy(ap.passphrase, pass);
    ap.open = open;
    ap.group_cipher = 4;
    ap.pairwise_cipher = 4;
    ap.on_air = true;
    if (!open)
        wpa_pmk(pass, strlen(pass), (const uint8_t*)ssid, strlen(ssid), ap.pmk);
    ap_set_gtk(1, 0x5A);
    queued = 0;
}

static struct WlanBss bss_of(bool open) {
    struct WlanBss b;
    memset(&b, 0, sizeof(b));
    memcpy(b.bssid, AP_MAC, 6);
    b.channel = 6;
    b.rssi = -50;
    b.security = open ? 0 : WLAN_SEC_WPA2;
    b.ssid_length = (uint8_t)strlen(ap.ssid);
    memcpy(b.ssid, ap.ssid, b.ssid_length);
    return b;
}

static int32_t connect_with(const char* pass) {
    struct WlanBss b = bss_of(ap.open);
    uint8_t pmk[32];
    if (pass != NULL)
        CHECK(wpa_pmk(pass, strlen(pass), b.ssid, b.ssid_length, pmk));
    return wlan_sta_connect(&sta, &b, pass != NULL ? pmk : NULL, now_ms);
}

static uint8_t link_state(void) {
    struct WlanLink l;
    wlan_sta_link(&sta, &l);
    return l.state;
}

static uint8_t link_detail(void) {
    struct WlanLink l;
    wlan_sta_link(&sta, &l);
    return l.detail;
}

static uint8_t link_reason(void) {
    struct WlanLink l;
    wlan_sta_link(&sta, &l);
    return l.reason;
}

// An Ethernet frame from the IP stack
static int32_t eth_send(const uint8_t* dst, uint16_t type, const char* text) {
    uint8_t f[1600];
    memcpy(f, dst, 6);
    memcpy(f + 6, STA_MAC, 6);
    put16be(f + 12, type);
    size_t n = strlen(text);
    memcpy(f + 14, text, n);
    return wlan_sta_send(&sta, f, (uint32_t)(14 + n));
}

static bool delivered_is(const uint8_t* dst, const uint8_t* src, uint16_t type, const char* text) {
    size_t n = strlen(text);
    return delivered_len == 14 + n && memcmp(delivered, dst, 6) == 0 && memcmp(delivered + 6, src, 6) == 0 &&
           get16be(delivered + 12) == type && memcmp(delivered + 14, text, n) == 0;
}

//
// Tests
//

static void test_open(void) {
    ap_reset("CafeOpen", "", true);
    CHECK(connect_with(NULL) == SYS_OK);
    CHECK(tuned_channel == 6 && link_state() == WLAN_LINK_JOINING);
    run(200);
    CHECK(link_state() == WLAN_LINK_CONNECTED && link_up);

    CHECK(eth_send(ROUTER, 0x0800, "hello open") == SYS_OK);
    CHECK(ap.got_count == 1 && ap.got_len == 10 && memcmp(ap.got, "hello open", 10) == 0 && memcmp(ap.got_dst, ROUTER, 6) == 0);

    ap_data(STA_MAC, ROUTER, 0x0800, (const uint8_t*)"reply", 5, false, false);
    run(100);
    CHECK(delivered_count == 1 && delivered_is(STA_MAC, ROUTER, 0x0800, "reply"));

    wlan_sta_disconnect(&sta, WLAN_REASON_USER);
    CHECK(link_state() == WLAN_LINK_IDLE && !link_up && ap.last_deauth == 3 && tuned_channel == 0);
    CHECK(link_reason() == WLAN_REASON_USER);
}

static void test_wpa2(void) {
    ap_reset("HomeNetwork", "correct horse battery", false);
    delivered_count = 0;
    CHECK(connect_with("correct horse battery") == SYS_OK);
    run(300);
    CHECK(ap.m2_ok && ap.m4_count == 1 && ap.keys);
    CHECK(link_state() == WLAN_LINK_CONNECTED && link_up);

    // Out: encrypted, and the AP reads it
    CHECK(eth_send(ROUTER, 0x0800, "secret ping") == SYS_OK);
    CHECK(ap.got_count == 1 && ap.got_len == 11 && memcmp(ap.got, "secret ping", 11) == 0);
    CHECK(eth_send(BCAST, 0x0806, "who has") == SYS_OK);
    CHECK(ap.got_count == 2 && memcmp(ap.got_dst, BCAST, 6) == 0);

    // In: unicast (plain and QoS data), and broadcast under the GTK
    ap_data(STA_MAC, ROUTER, 0x0800, (const uint8_t*)"pong", 4, true, false);
    run(50);
    CHECK(delivered_count == 1 && delivered_is(STA_MAC, ROUTER, 0x0800, "pong"));
    ap_data(STA_MAC, ROUTER, 0x0800, (const uint8_t*)"qos pong", 8, true, true);
    run(50);
    CHECK(delivered_count == 2 && delivered_is(STA_MAC, ROUTER, 0x0800, "qos pong"));
    ap_data(BCAST, ROUTER, 0x0806, (const uint8_t*)"arp", 3, true, false);
    run(50);
    CHECK(delivered_count == 3 && delivered_is(BCAST, ROUTER, 0x0806, "arp"));

    // A replayed frame, a tampered one, and one in the clear are dropped
    ap_data(STA_MAC, ROUTER, 0x0800, (const uint8_t*)"once", 4, true, false);
    uint8_t copy[2400];
    uint32_t copy_len = queue[0].len;
    memcpy(copy, queue[0].data, copy_len);
    run(50);
    CHECK(delivered_count == 4);
    to_sta(copy, copy_len);
    run(50);
    CHECK(delivered_count == 4);
    ap_data(STA_MAC, ROUTER, 0x0800, (const uint8_t*)"tampered", 8, true, false);
    queue[0].data[40] ^= 0x01;
    run(50);
    CHECK(delivered_count == 4);
    ap_data(STA_MAC, ROUTER, 0x0800, (const uint8_t*)"plain", 5, false, false);
    run(50);
    CHECK(delivered_count == 4);
    // A forged EAPOL frame in the clear (no valid MIC) changes nothing
    uint8_t e[99];
    memset(e, 0, sizeof(e));
    e[0] = 2; e[1] = 3; put16be(e + 2, 95); e[4] = 2; put16be(e + 5, 0x0008 | 0x0080 | 0x0100 | 0x0040 | 0x1000 | 2);
    e[16] = 0x7F;
    ap_data(STA_MAC, AP_MAC, 0x888E, e, sizeof(e), false, false);
    run(50);
    CHECK(link_state() == WLAN_LINK_CONNECTED);

    // Group rekey: new GTK under key id 2, answered with group message 2
    ap_set_gtk(2, 0x77);
    uint8_t data[64];
    uint32_t n = gtk_data(data, false);
    ap_key(0x0080 | 0x0100 | 0x0200 | 0x1000, NULL, data, n, true);
    run(50);
    CHECK(ap.group2_count == 1);
    ap_data(BCAST, ROUTER, 0x0806, (const uint8_t*)"new gtk", 7, true, false);
    run(50);
    CHECK(delivered_count == 5 && delivered_is(BCAST, ROUTER, 0x0806, "new gtk"));

    // Beacons keep it up; their silence ends it
    for (int i = 0; i < 10; i++) {
        ap_beacon();
        run(1000);
    }
    CHECK(link_state() == WLAN_LINK_CONNECTED);
    run(9000);
    CHECK(link_state() == WLAN_LINK_IDLE && link_reason() == WLAN_REASON_LOST && !link_up);
    CHECK(eth_send(ROUTER, 0x0800, "gone") == SYS_EAGAIN);
}

static void test_wrong_password(void) {
    ap_reset("HomeNetwork", "correct horse battery", false);
    CHECK(connect_with("incorrect horse") == SYS_OK);
    run(2000);
    CHECK(!ap.m2_ok && ap.m2_count >= 1 && !link_up);
    CHECK(link_state() == WLAN_LINK_IDLE && link_reason() == WLAN_REASON_PASSWORD);

    // An AP that just stops answering after M1: the handshake times out
    ap_reset("HomeNetwork", "correct horse battery", false);
    ap.silent = true;               // No retries, no deauth
    CHECK(connect_with("incorrect horse") == SYS_OK);
    run(3000);
    CHECK(link_state() == WLAN_LINK_HANDSHAKE);
    run(5000);
    CHECK(link_state() == WLAN_LINK_IDLE && link_reason() == WLAN_REASON_PASSWORD && ap.last_deauth == 3);
}

static void test_lost_m4(void) {
    ap_reset("HomeNetwork", "correct horse battery", false);
    ap.drop_next_m4 = true;
    CHECK(connect_with("correct horse battery") == SYS_OK);
    run(300);
    CHECK(ap.m4_count == 2 && ap.keys && link_state() == WLAN_LINK_CONNECTED);
    CHECK(eth_send(ROUTER, 0x0800, "after") == SYS_OK && ap.got_count == 1);

    // The AP deauthenticates: link down, reason says so
    uint8_t f[64];
    uint32_t n = ap_header(f, 0xC0, 0, STA_MAC, AP_MAC);
    put16le(f + n, 1);
    to_sta(f, n + 2);
    run(50);
    CHECK(link_state() == WLAN_LINK_IDLE && link_reason() == WLAN_REASON_LEFT && !link_up);

    // A deauth from someone else's BSSID is ignored
    ap_reset("HomeNetwork", "correct horse battery", false);
    CHECK(connect_with("correct horse battery") == SYS_OK);
    run(300);
    n = ap_header(f, 0xC0, 0, STA_MAC, ROUTER);
    memcpy(f + 10, ROUTER, 6);
    put16le(f + n, 1);
    to_sta(f, n + 2);
    run(50);
    CHECK(link_state() == WLAN_LINK_CONNECTED);
    wlan_sta_disconnect(&sta, WLAN_REASON_USER);
}

// WPA/WPA2 mixed: CCMP for our traffic, TKIP for the AP's broadcasts
static void test_mixed(void) {
    ap_reset("Mixed", "correct horse battery", false);
    ap.group_cipher = 2;
    delivered_count = 0;
    CHECK(connect_with("correct horse battery") == SYS_OK);
    run(300);
    CHECK(ap.m2_ok && ap.keys && link_state() == WLAN_LINK_CONNECTED && link_up);
    CHECK(sta.group_tkip);

    // Ours still go out CCMP (the AP decrypts them as such), unicast in too
    CHECK(eth_send(ROUTER, 0x0800, "dhcp discover") == SYS_OK);
    CHECK(ap.got_count == 1 && memcmp(ap.got, "dhcp discover", 13) == 0);
    CHECK(eth_send(BCAST, 0x0806, "who has") == SYS_OK && ap.got_count == 2);
    ap_data(STA_MAC, ROUTER, 0x0800, (const uint8_t*)"pong", 4, true, false);
    run(50);
    CHECK(delivered_count == 1 && delivered_is(STA_MAC, ROUTER, 0x0800, "pong"));

    // Broadcasts in TKIP: plain and QoS data
    ap_data(BCAST, ROUTER, 0x0806, (const uint8_t*)"arp request", 11, true, false);
    run(50);
    CHECK(delivered_count == 2 && delivered_is(BCAST, ROUTER, 0x0806, "arp request"));
    ap_data(BCAST, ROUTER, 0x0800, (const uint8_t*)"dhcp offer", 10, true, true);
    run(50);
    CHECK(delivered_count == 3 && delivered_is(BCAST, ROUTER, 0x0800, "dhcp offer"));

    // A replay, a flipped bit, a bad Michael MIC (good ICV), and one in the clear: dropped
    ap_data(BCAST, ROUTER, 0x0806, (const uint8_t*)"once", 4, true, false);
    uint8_t copy[2400];
    uint32_t copy_len = queue[0].len;
    memcpy(copy, queue[0].data, copy_len);
    run(50);
    CHECK(delivered_count == 4);
    to_sta(copy, copy_len);
    run(50);
    CHECK(delivered_count == 4);
    ap_data(BCAST, ROUTER, 0x0806, (const uint8_t*)"tampered", 8, true, false);
    queue[0].data[40] ^= 0x01;
    run(50);
    CHECK(delivered_count == 4);
    uint8_t mic_key = ap.gtk_raw[16];
    ap.gtk_raw[16] ^= 0xFF;
    ap_data(BCAST, ROUTER, 0x0806, (const uint8_t*)"forged", 6, true, false);
    ap.gtk_raw[16] = mic_key;
    run(50);
    CHECK(delivered_count == 4);
    ap_data(BCAST, ROUTER, 0x0806, (const uint8_t*)"plain", 5, false, false);
    run(50);
    CHECK(delivered_count == 4);
    // A frame of TKIP's minimum size and less
    ap_data(BCAST, ROUTER, 0x0806, (const uint8_t*)"", 0, true, false);
    queue[0].len = 24 + 8 + 19;
    run(50);
    CHECK(delivered_count == 4 && link_state() == WLAN_LINK_CONNECTED);

    // Group rekey to a new 32-byte TKIP key
    ap_set_gtk(2, 0x33);
    uint8_t data[96];
    uint32_t n = gtk_data(data, false);
    ap_key(0x0080 | 0x0100 | 0x0200 | 0x1000, NULL, data, n, true);
    run(50);
    CHECK(ap.group2_count == 1);
    ap_data(BCAST, ROUTER, 0x0806, (const uint8_t*)"new gtk", 7, true, false);
    run(50);
    CHECK(delivered_count == 5 && delivered_is(BCAST, ROUTER, 0x0806, "new gtk"));
    wlan_sta_disconnect(&sta, WLAN_REASON_USER);
    CHECK(!link_up);

    // Then a CCMP network again: the TKIP choice doesn't stick
    ap_reset("HomeNetwork", "correct horse battery", false);
    CHECK(connect_with("correct horse battery") == SYS_OK);
    run(300);
    CHECK(link_state() == WLAN_LINK_CONNECTED && !sta.group_tkip);
    ap_data(BCAST, ROUTER, 0x0806, (const uint8_t*)"ccmp arp", 8, true, false);
    run(50);
    CHECK(delivered_count == 6 && delivered_is(BCAST, ROUTER, 0x0806, "ccmp arp"));
    wlan_sta_disconnect(&sta, WLAN_REASON_USER);
}

static void test_refused(void) {
    // TKIP only (no CCMP for our own traffic): refused at its beacon
    ap_reset("Old", "correct horse battery", false);
    ap.group_cipher = 2;
    ap.pairwise_cipher = 2;
    CHECK(connect_with("correct horse battery") == SYS_OK);
    run(200);
    CHECK(link_state() == WLAN_LINK_IDLE && link_reason() == WLAN_REASON_UNSUPPORTED);
    CHECK(link_detail() == WLAN_DETAIL_PAIRWISE);

    // A WEP group cipher
    ap_reset("Wep", "correct horse battery", false);
    ap.group_cipher = 1;
    CHECK(connect_with("correct horse battery") == SYS_OK);
    run(200);
    CHECK(link_reason() == WLAN_REASON_UNSUPPORTED && link_detail() == WLAN_DETAIL_GROUP);

    // Management frame protection required
    ap_reset("Strict", "correct horse battery", false);
    ap.pmf_required = true;
    CHECK(connect_with("correct horse battery") == SYS_OK);
    run(200);
    CHECK(link_reason() == WLAN_REASON_UNSUPPORTED && link_detail() == WLAN_DETAIL_PMF);

    // Message 3's RSN element differs from the beacon's: a downgrade
    ap_reset("HomeNetwork", "correct horse battery", false);
    ap.m3_bad_rsn = true;
    CHECK(connect_with("correct horse battery") == SYS_OK);
    run(300);
    CHECK(link_state() == WLAN_LINK_IDLE && link_reason() == WLAN_REASON_UNSUPPORTED && !ap.keys);
    CHECK(link_detail() == WLAN_DETAIL_CHANGED);
    // A later attempt starts with no detail
    ap_reset("HomeNetwork", "correct horse battery", false);
    CHECK(connect_with("correct horse battery") == SYS_OK && link_detail() == WLAN_DETAIL_NONE);
    run(300);
    CHECK(link_state() == WLAN_LINK_CONNECTED);
    wlan_sta_disconnect(&sta, WLAN_REASON_USER);
    CHECK(link_detail() == WLAN_DETAIL_NONE);

    // WPA3 only, WEP, enterprise: refused before anything is sent
    struct WlanBss b = bss_of(false);
    uint8_t pmk[32] = { 0 };
    b.security = WLAN_SEC_WPA3;
    CHECK(wlan_sta_connect(&sta, &b, pmk, now_ms) == SYS_EINVAL && link_reason() == WLAN_REASON_UNSUPPORTED);
    CHECK(link_detail() == WLAN_DETAIL_AKM);
    b.security = WLAN_SEC_WEP;
    CHECK(wlan_sta_connect(&sta, &b, pmk, now_ms) == SYS_EINVAL && link_detail() == WLAN_DETAIL_GROUP);
    b.security = WLAN_SEC_WPA2 | WLAN_SEC_ENTERPRISE;
    CHECK(wlan_sta_connect(&sta, &b, pmk, now_ms) == SYS_EINVAL && link_detail() == WLAN_DETAIL_AKM);
    b.security = WLAN_SEC_WPA;
    CHECK(wlan_sta_connect(&sta, &b, pmk, now_ms) == SYS_EINVAL && link_detail() == WLAN_DETAIL_NO_RSN);
    b.security = WLAN_SEC_WPA2 | WLAN_SEC_WPA3;     // Transition mode: fine
    ap_reset("HomeNetwork", "correct horse battery", false);
    b = bss_of(false);
    b.security = WLAN_SEC_WPA2 | WLAN_SEC_WPA3;
    wpa_pmk("correct horse battery", 21, b.ssid, b.ssid_length, pmk);
    CHECK(wlan_sta_connect(&sta, &b, pmk, now_ms) == SYS_OK);
    run(300);
    CHECK(link_state() == WLAN_LINK_CONNECTED);
    wlan_sta_disconnect(&sta, WLAN_REASON_USER);
    // Secured with no PMK
    CHECK(wlan_sta_connect(&sta, &b, NULL, now_ms) == SYS_EINVAL);

    // Not on the air: not found
    ap_reset("Gone", "correct horse battery", false);
    ap.on_air = false;
    CHECK(connect_with("correct horse battery") == SYS_OK);
    run(2000);
    CHECK(link_state() == WLAN_LINK_IDLE && link_reason() == WLAN_REASON_NOT_FOUND);

    // The radio won't tune
    ap_reset("HomeNetwork", "correct horse battery", false);
    tune_fail = true;
    CHECK(connect_with("correct horse battery") == SYS_EIO && link_reason() == WLAN_REASON_RADIO);
    tune_fail = false;
}

// Garbage of every length into every state: nothing read past the end
// (the address sanitizer watches)
static void test_garbage(void) {
    ap_reset("HomeNetwork", "correct horse battery", false);
    CHECK(connect_with("correct horse battery") == SYS_OK);
    run(300);
    uint8_t f[600];
    srand(7);
    for (int round = 0; round < 20000; round++) {
        uint32_t len = (uint32_t)(rand() % 300);
        for (uint32_t i = 0; i < len; i++) f[i] = (uint8_t)rand();
        if (len >= 24 && (round & 1)) {
            // Look like it's from the AP, to get past the address checks
            f[0] = (uint8_t)((round & 2) ? 0x08 : (rand() & 0xF0));
            f[1] = (uint8_t)(0x02 | (rand() & 0x40));
            memcpy(f + 4, STA_MAC, 6);
            memcpy(f + 10, AP_MAC, 6);
            if (!(round & 2)) memcpy(f + 16, AP_MAC, 6);
            if (len >= 32 && (round & 4)) {
                memcpy(f + 24, "\xAA\xAA\x03\x00\x00\x00\x88\x8E", 8);
                f[1] = 0x02;
            }
        }
        uint8_t* p = malloc(len ? len : 1);        // Exact size, so overreads trip
        memcpy(p, f, len);
        wlan_sta_rx(&sta, p, len, -60, now_ms);
        free(p);
    }
    CHECK(link_state() == WLAN_LINK_CONNECTED || link_state() == WLAN_LINK_IDLE);
}

int main(void) {
    crypt_random_add("sta test", 8);
    wlan_sta_init(&sta, &ops, NULL, STA_MAC);
    test_open();
    test_wpa2();
    test_wrong_password();
    test_lost_m4();
    test_mixed();
    test_refused();
    test_garbage();
    printf("wlan_sta: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
