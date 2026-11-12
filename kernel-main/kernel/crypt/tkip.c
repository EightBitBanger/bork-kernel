#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/crypt/crypt.h>
#include <kernel/crypt/tkip.h>
#include <kernel/util/string.h>

//
// TKIP receive (tkip.h): the key mixing, RC4, the CRC-32 ICV and Michael
//

#define FC1_MOREFRAG    0x04
#define EXT_IV          0x20

// The key mixing's S-box (802.11-2020 12.5.2.5): its high and low bytes
// swapped give the second table
static const uint16_t sbox[256] = {
    0xC6A5, 0xF884, 0xEE99, 0xF68D, 0xFF0D, 0xD6BD, 0xDEB1, 0x9154,
    0x6050, 0x0203, 0xCEA9, 0x567D, 0xE719, 0xB562, 0x4DE6, 0xEC9A,
    0x8F45, 0x1F9D, 0x8940, 0xFA87, 0xEF15, 0xB2EB, 0x8EC9, 0xFB0B,
    0x41EC, 0xB367, 0x5FFD, 0x45EA, 0x23BF, 0x53F7, 0xE496, 0x9B5B,
    0x75C2, 0xE11C, 0x3DAE, 0x4C6A, 0x6C5A, 0x7E41, 0xF502, 0x834F,
    0x685C, 0x51F4, 0xD134, 0xF908, 0xE293, 0xAB73, 0x6253, 0x2A3F,
    0x080C, 0x9552, 0x4665, 0x9D5E, 0x3028, 0x37A1, 0x0A0F, 0x2FB5,
    0x0E09, 0x2436, 0x1B9B, 0xDF3D, 0xCD26, 0x4E69, 0x7FCD, 0xEA9F,
    0x121B, 0x1D9E, 0x5874, 0x342E, 0x362D, 0xDCB2, 0xB4EE, 0x5BFB,
    0xA4F6, 0x764D, 0xB761, 0x7DCE, 0x527B, 0xDD3E, 0x5E71, 0x1397,
    0xA6F5, 0xB968, 0x0000, 0xC12C, 0x4060, 0xE31F, 0x79C8, 0xB6ED,
    0xD4BE, 0x8D46, 0x67D9, 0x724B, 0x94DE, 0x98D4, 0xB0E8, 0x854A,
    0xBB6B, 0xC52A, 0x4FE5, 0xED16, 0x86C5, 0x9AD7, 0x6655, 0x1194,
    0x8ACF, 0xE910, 0x0406, 0xFE81, 0xA0F0, 0x7844, 0x25BA, 0x4BE3,
    0xA2F3, 0x5DFE, 0x80C0, 0x058A, 0x3FAD, 0x21BC, 0x7048, 0xF104,
    0x63DF, 0x77C1, 0xAF75, 0x4263, 0x2030, 0xE51A, 0xFD0E, 0xBF6D,
    0x814C, 0x1814, 0x2635, 0xC32F, 0xBEE1, 0x35A2, 0x88CC, 0x2E39,
    0x9357, 0x55F2, 0xFC82, 0x7A47, 0xC8AC, 0xBAE7, 0x322B, 0xE695,
    0xC0A0, 0x1998, 0x9ED1, 0xA37F, 0x4466, 0x547E, 0x3BAB, 0x0B83,
    0x8CCA, 0xC729, 0x6BD3, 0x283C, 0xA779, 0xBCE2, 0x161D, 0xAD76,
    0xDB3B, 0x6456, 0x744E, 0x141E, 0x92DB, 0x0C0A, 0x486C, 0xB8E4,
    0x9F5D, 0xBD6E, 0x43EF, 0xC4A6, 0x39A8, 0x31A4, 0xD337, 0xF28B,
    0xD532, 0x8B43, 0x6E59, 0xDAB7, 0x018C, 0xB164, 0x9CD2, 0x49E0,
    0xD8B4, 0xACFA, 0xF307, 0xCF25, 0xCAAF, 0xF48E, 0x47E9, 0x1018,
    0x6FD5, 0xF088, 0x4A6F, 0x5C72, 0x3824, 0x57F1, 0x73C7, 0x9751,
    0xCB23, 0xA17C, 0xE89C, 0x3E21, 0x96DD, 0x61DC, 0x0D86, 0x0F85,
    0xE090, 0x7C42, 0x71C4, 0xCCAA, 0x90D8, 0x0605, 0xF701, 0x1C12,
    0xC2A3, 0x6A5F, 0xAEF9, 0x69D0, 0x1791, 0x9958, 0x3A27, 0x27B9,
    0xD938, 0xEB13, 0x2BB3, 0x2233, 0xD2BB, 0xA970, 0x0789, 0x33A7,
    0x2DB6, 0x3C22, 0x1592, 0xC920, 0x8749, 0xAAFF, 0x5078, 0xA57A,
    0x038F, 0x59F8, 0x0980, 0x1A17, 0x65DA, 0xD731, 0x84C6, 0xD0B8,
    0x82C3, 0x29B0, 0x5A77, 0x1E11, 0x7BCB, 0xA8FC, 0x6DD6, 0x2C3A,
};

static uint16_t mk16(uint8_t hi, uint8_t lo) { return (uint16_t)((hi << 8) | lo); }

static uint16_t s16(uint16_t v) {
    uint16_t b = sbox[v >> 8];
    return (uint16_t)(sbox[v & 0xFF] ^ (uint16_t)((b >> 8) | (b << 8)));
}

static uint16_t rotr1(uint16_t v) { return (uint16_t)((v >> 1) | (v << 15)); }

void tkip_mix(const uint8_t tk[TKIP_TK_LEN], const uint8_t ta[6], uint64_t tsc, uint8_t rc4_key[16]) {
    uint16_t iv16 = (uint16_t)tsc;
    uint32_t iv32 = (uint32_t)(tsc >> 16);

    // Phase 1: the TTAK from the TK, our transmitter and the TSC's top 32 bits
    uint16_t p[6];
    p[0] = (uint16_t)iv32;
    p[1] = (uint16_t)(iv32 >> 16);
    p[2] = mk16(ta[1], ta[0]);
    p[3] = mk16(ta[3], ta[2]);
    p[4] = mk16(ta[5], ta[4]);
    for (int i = 0; i < 8; i++) {
        int j = 2 * (i & 1);
        p[0] = (uint16_t)(p[0] + s16((uint16_t)(p[4] ^ mk16(tk[1 + j], tk[0 + j]))));
        p[1] = (uint16_t)(p[1] + s16((uint16_t)(p[0] ^ mk16(tk[5 + j], tk[4 + j]))));
        p[2] = (uint16_t)(p[2] + s16((uint16_t)(p[1] ^ mk16(tk[9 + j], tk[8 + j]))));
        p[3] = (uint16_t)(p[3] + s16((uint16_t)(p[2] ^ mk16(tk[13 + j], tk[12 + j]))));
        p[4] = (uint16_t)(p[4] + s16((uint16_t)(p[3] ^ mk16(tk[1 + j], tk[0 + j]))) + i);
    }

    // Phase 2: the per-packet key, with the low 16 bits
    p[5] = (uint16_t)(p[4] + iv16);
    p[0] = (uint16_t)(p[0] + s16((uint16_t)(p[5] ^ mk16(tk[1], tk[0]))));
    p[1] = (uint16_t)(p[1] + s16((uint16_t)(p[0] ^ mk16(tk[3], tk[2]))));
    p[2] = (uint16_t)(p[2] + s16((uint16_t)(p[1] ^ mk16(tk[5], tk[4]))));
    p[3] = (uint16_t)(p[3] + s16((uint16_t)(p[2] ^ mk16(tk[7], tk[6]))));
    p[4] = (uint16_t)(p[4] + s16((uint16_t)(p[3] ^ mk16(tk[9], tk[8]))));
    p[5] = (uint16_t)(p[5] + s16((uint16_t)(p[4] ^ mk16(tk[11], tk[10]))));
    p[0] = (uint16_t)(p[0] + rotr1((uint16_t)(p[5] ^ mk16(tk[13], tk[12]))));
    p[1] = (uint16_t)(p[1] + rotr1((uint16_t)(p[0] ^ mk16(tk[15], tk[14]))));
    p[2] = (uint16_t)(p[2] + rotr1(p[1]));
    p[3] = (uint16_t)(p[3] + rotr1(p[2]));
    p[4] = (uint16_t)(p[4] + rotr1(p[3]));
    p[5] = (uint16_t)(p[5] + rotr1(p[4]));

    rc4_key[0] = (uint8_t)(iv16 >> 8);
    rc4_key[1] = (uint8_t)(((iv16 >> 8) | 0x20) & 0x7F);
    rc4_key[2] = (uint8_t)iv16;
    rc4_key[3] = (uint8_t)(((p[5] ^ mk16(tk[1], tk[0])) >> 1) & 0xFF);
    for (int i = 0; i < 6; i++) {
        rc4_key[4 + 2 * i] = (uint8_t)p[i];
        rc4_key[5 + 2 * i] = (uint8_t)(p[i] >> 8);
    }
    crypt_wipe(p, sizeof(p));
}

void tkip_rc4(const uint8_t key[16], const uint8_t* in, size_t len, uint8_t* out) {
    uint8_t st[256];
    for (int i = 0; i < 256; i++)
        st[i] = (uint8_t)i;
    uint8_t j = 0;
    for (int i = 0; i < 256; i++) {
        j = (uint8_t)(j + st[i] + key[i & 15]);
        uint8_t t = st[i]; st[i] = st[j]; st[j] = t;
    }
    uint8_t a = 0, b = 0;
    for (size_t n = 0; n < len; n++) {
        a = (uint8_t)(a + 1);
        b = (uint8_t)(b + st[a]);
        uint8_t t = st[a]; st[a] = st[b]; st[b] = t;
        out[n] = (uint8_t)(in[n] ^ st[(uint8_t)(st[a] + st[b])]);
    }
    crypt_wipe(st, sizeof(st));
}

uint32_t tkip_crc32(const uint8_t* data, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= data[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

//
// Michael, fed in pieces
//

typedef struct {
    uint32_t l, r, block;
    uint32_t have;          // Bytes in `block`
} michael_t;

static uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
static uint32_t ror32(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }
static uint32_t xswap(uint32_t v) { return ((v & 0xFF00FF00u) >> 8) | ((v & 0x00FF00FFu) << 8); }
static uint32_t get32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void michael_block(michael_t* m, uint32_t w) {
    m->l ^= w;
    m->r ^= rol32(m->l, 17);
    m->l += m->r;
    m->r ^= xswap(m->l);
    m->l += m->r;
    m->r ^= rol32(m->l, 3);
    m->l += m->r;
    m->r ^= ror32(m->l, 2);
    m->l += m->r;
}

static void michael_init(michael_t* m, const uint8_t key[8]) {
    m->l = get32le(key);
    m->r = get32le(key + 4);
    m->block = 0;
    m->have = 0;
}

static void michael_add(michael_t* m, const uint8_t* p, size_t len) {
    for (size_t i = 0; i < len; i++) {
        m->block |= (uint32_t)p[i] << (8 * m->have);
        if (++m->have == 4) {
            michael_block(m, m->block);
            m->block = 0;
            m->have = 0;
        }
    }
}

static void michael_end(michael_t* m, uint8_t mic[8]) {
    // 0x5A, then zeros to a whole word, then a word of zeros
    uint8_t pad = 0x5A, zero = 0;
    michael_add(m, &pad, 1);
    while (m->have != 0)
        michael_add(m, &zero, 1);
    michael_block(m, 0);
    for (int i = 0; i < 4; i++) {
        mic[i]     = (uint8_t)(m->l >> (8 * i));
        mic[4 + i] = (uint8_t)(m->r >> (8 * i));
    }
    crypt_wipe(m, sizeof(*m));
}

void tkip_michael(const uint8_t key[8], const uint8_t* data, size_t len, uint8_t mic[8]) {
    michael_t m;
    michael_init(&m, key);
    michael_add(&m, data, len);
    michael_end(&m, mic);
}

void tkip_set_group_key(tkip_key_t* key, const uint8_t gtk[TKIP_GTK_LEN]) {
    memcpy(key->tk, gtk, TKIP_TK_LEN);
    memcpy(key->rx_mic, gtk + 16, 8);       // The authenticator's Tx MIC key
}

bool tkip_decrypt(const tkip_key_t* key, const uint8_t* frame, size_t frame_len, size_t hdr_len,
                  const uint8_t* da, const uint8_t* sa, uint8_t priority,
                  uint8_t* out, size_t* out_len, uint64_t* tsc, uint8_t* key_id) {
    if (hdr_len < 24 || frame_len < hdr_len + TKIP_IV_LEN + TKIP_MIC_LEN + TKIP_ICV_LEN)
        return false;
    if (frame[1] & FC1_MOREFRAG || (frame[22] & 0x0F) != 0)
        return false;               // Fragments: the MIC covers the whole MSDU (not taken)
    const uint8_t* iv = frame + hdr_len;
    if (!(iv[3] & EXT_IV) || iv[1] != ((iv[0] | 0x20) & 0x7F))
        return false;
    uint64_t t = (uint64_t)iv[2] | ((uint64_t)iv[0] << 8) | ((uint64_t)iv[4] << 16) |
                 ((uint64_t)iv[5] << 24) | ((uint64_t)iv[6] << 32) | ((uint64_t)iv[7] << 40);

    uint8_t rc4_key[16];
    tkip_mix(key->tk, frame + 10, t, rc4_key);      // Transmitter: address 2
    size_t enc_len = frame_len - hdr_len - TKIP_IV_LEN;
    tkip_rc4(rc4_key, iv + TKIP_IV_LEN, enc_len, out);
    crypt_wipe(rc4_key, sizeof(rc4_key));

    size_t msdu = enc_len - TKIP_MIC_LEN - TKIP_ICV_LEN;
    uint32_t icv = get32le(out + msdu + TKIP_MIC_LEN);
    bool ok = tkip_crc32(out, msdu + TKIP_MIC_LEN) == icv;

    uint8_t mic[8];
    if (ok) {
        uint8_t hdr[16] = { 0 };
        memcpy(hdr, da, 6);
        memcpy(hdr + 6, sa, 6);
        hdr[12] = priority;
        michael_t m;
        michael_init(&m, key->rx_mic);
        michael_add(&m, hdr, sizeof(hdr));
        michael_add(&m, out, msdu);
        michael_end(&m, mic);
        ok = crypt_equal(mic, out + msdu, TKIP_MIC_LEN);
    }
    if (!ok) {
        crypt_wipe(out, enc_len);
        return false;
    }
    crypt_wipe(out + msdu, TKIP_MIC_LEN + TKIP_ICV_LEN);
    *out_len = msdu;
    *tsc     = t;
    *key_id  = (uint8_t)(iv[3] >> 6);
    return true;
}
