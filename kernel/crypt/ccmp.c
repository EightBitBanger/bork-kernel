#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/crypt/ccmp.h>
#include <kernel/crypt/crypt.h>
#include <kernel/util/string.h>

//
// CCMP: the CCM mode of AES (RFC 3610 with M = 8, L = 2), with the nonce
// and additional authenticated data (AAD) built from the 802.11 header
//

#define FC0_TYPE_MASK   0x0C
#define FC0_TYPE_DATA   0x08
#define FC0_QOS         0x80        // A data subtype with the QoS Control field
#define FC1_TODS        0x01
#define FC1_FROMDS      0x02
#define FC1_RETRY       0x08
#define FC1_PWRMGT      0x10
#define FC1_MOREDATA    0x20
#define FC1_PROTECTED   0x40
#define FC1_ORDER       0x80

#define AAD_MAX         30

// The AAD and nonce for a header. False: too short for what its frame
// control says it holds.
static bool build(const uint8_t* hdr, size_t hdr_len, uint64_t pn, uint8_t aad[AAD_MAX], size_t* aad_len,
                  uint8_t nonce[13]) {
    if (hdr_len < 24)
        return false;
    bool data = (hdr[0] & FC0_TYPE_MASK) == FC0_TYPE_DATA;
    bool qos  = data && (hdr[0] & FC0_QOS);
    bool a4   = (hdr[1] & (FC1_TODS | FC1_FROMDS)) == (FC1_TODS | FC1_FROMDS);
    size_t need = 24 + (a4 ? 6 : 0) + (qos ? 2 : 0);
    if (hdr_len < need)
        return false;

    size_t n = 0;
    aad[n++] = data ? (uint8_t)(hdr[0] & 0x8F) : hdr[0];    // Data: subtype bits 4-6 masked
    uint8_t fc1 = (uint8_t)((hdr[1] & ~(FC1_RETRY | FC1_PWRMGT | FC1_MOREDATA)) | FC1_PROTECTED);
    if (qos)
        fc1 &= (uint8_t)~FC1_ORDER;
    aad[n++] = fc1;
    memcpy(aad + n, hdr + 4, 18);           // A1, A2, A3
    n += 18;
    aad[n++] = hdr[22] & 0x0F;              // Fragment number only
    aad[n++] = 0;
    if (a4) {
        memcpy(aad + n, hdr + 24, 6);
        n += 6;
    }
    uint8_t tid = 0;
    if (qos) {
        const uint8_t* qc = hdr + (a4 ? 30 : 24);
        tid = qc[0] & 0x0F;
        aad[n++] = tid;
        aad[n++] = 0;
    }
    *aad_len = n;

    nonce[0] = tid;
    memcpy(nonce + 1, hdr + 10, 6);         // A2: the sender
    for (int i = 0; i < 6; i++)
        nonce[7 + i] = (uint8_t)(pn >> (8 * (5 - i)));
    return true;
}

static void xor_block(uint8_t* x, const uint8_t* y, size_t n) {
    for (size_t i = 0; i < n; i++)
        x[i] ^= y[i];
}

// CBC-MAC over B0, the AAD and the body: the 16-byte tag before encryption
static void cbc_mac(const aes128_t* tk, const uint8_t nonce[13], const uint8_t* aad, size_t aad_len,
                    const uint8_t* body, size_t len, uint8_t x[16]) {
    uint8_t b[16];
    b[0] = 0x59;                            // Adata, M = 8, L = 2
    memcpy(b + 1, nonce, 13);
    b[14] = (uint8_t)(len >> 8);
    b[15] = (uint8_t)len;
    aes128_encrypt(tk, b, x);

    // The AAD, after its length, in 16-byte blocks (it is at most 30 bytes)
    uint8_t a[32];
    memset(a, 0, sizeof(a));
    a[0] = (uint8_t)(aad_len >> 8);
    a[1] = (uint8_t)aad_len;
    memcpy(a + 2, aad, aad_len);
    for (size_t off = 0; off < 2 + aad_len; off += 16) {
        xor_block(x, a + off, 16);
        aes128_encrypt(tk, x, x);
    }
    for (size_t off = 0; off < len; off += 16) {
        size_t n = len - off < 16 ? len - off : 16;
        xor_block(x, body + off, n);        // The last block zero padded
        aes128_encrypt(tk, x, x);
    }
    crypt_wipe(b, sizeof(b));
}

// The counter block's keystream for block i
static void counter(const aes128_t* tk, const uint8_t nonce[13], uint16_t i, uint8_t out[16]) {
    uint8_t a[16];
    a[0] = 0x01;                            // L = 2
    memcpy(a + 1, nonce, 13);
    a[14] = (uint8_t)(i >> 8);
    a[15] = (uint8_t)i;
    aes128_encrypt(tk, a, out);
}

// Counter mode over `len` bytes from `in` to `out` (may be the same)
static void ctr(const aes128_t* tk, const uint8_t nonce[13], const uint8_t* in, size_t len, uint8_t* out) {
    uint8_t s[16];
    uint16_t i = 1;
    for (size_t off = 0; off < len; off += 16, i++) {
        size_t n = len - off < 16 ? len - off : 16;
        counter(tk, nonce, i, s);
        for (size_t k = 0; k < n; k++)
            out[off + k] = in[off + k] ^ s[k];
    }
    crypt_wipe(s, sizeof(s));
}

bool ccmp_encrypt(const aes128_t* tk, const uint8_t* hdr, size_t hdr_len, uint64_t pn, uint8_t key_id,
                  const uint8_t* body, size_t len, uint8_t* out) {
    uint8_t aad[AAD_MAX], nonce[13], x[16], s0[16];
    size_t aad_len;
    if (len > 0xFFFF || pn > CCMP_PN_MAX || !build(hdr, hdr_len, pn, aad, &aad_len, nonce))
        return false;
    cbc_mac(tk, nonce, aad, aad_len, body, len, x);     // Before `out` is written: it may overlap
    memmove(out + CCMP_HDR_LEN, body, len);

    uint8_t* p = out;
    p[0] = (uint8_t)pn;
    p[1] = (uint8_t)(pn >> 8);
    p[2] = 0;
    p[3] = (uint8_t)(0x20 | ((key_id & 3) << 6));       // ExtIV
    p[4] = (uint8_t)(pn >> 16);
    p[5] = (uint8_t)(pn >> 24);
    p[6] = (uint8_t)(pn >> 32);
    p[7] = (uint8_t)(pn >> 40);
    ctr(tk, nonce, out + CCMP_HDR_LEN, len, out + CCMP_HDR_LEN);
    counter(tk, nonce, 0, s0);
    for (int i = 0; i < CCMP_MIC_LEN; i++)
        out[CCMP_HDR_LEN + len + i] = x[i] ^ s0[i];
    crypt_wipe(x, sizeof(x));
    crypt_wipe(s0, sizeof(s0));
    return true;
}

bool ccmp_decrypt(const aes128_t* tk, const uint8_t* frame, size_t frame_len, size_t hdr_len, uint8_t* out,
                  size_t* out_len, uint64_t* pn, uint8_t* key_id) {
    if (frame_len < hdr_len + CCMP_HDR_LEN + CCMP_MIC_LEN)
        return false;
    const uint8_t* c = frame + hdr_len;
    if (!(c[3] & 0x20))
        return false;                       // No ExtIV: not CCMP
    uint64_t n = (uint64_t)c[0] | ((uint64_t)c[1] << 8) | ((uint64_t)c[4] << 16) | ((uint64_t)c[5] << 24) |
                 ((uint64_t)c[6] << 32) | ((uint64_t)c[7] << 40);
    size_t len = frame_len - hdr_len - CCMP_HDR_LEN - CCMP_MIC_LEN;
    uint8_t aad[AAD_MAX], nonce[13], x[16], s0[16], mic[CCMP_MIC_LEN];
    size_t aad_len;
    if (len > 0xFFFF || !build(frame, hdr_len, n, aad, &aad_len, nonce))
        return false;

    ctr(tk, nonce, c + CCMP_HDR_LEN, len, out);
    cbc_mac(tk, nonce, aad, aad_len, out, len, x);
    counter(tk, nonce, 0, s0);
    for (int i = 0; i < CCMP_MIC_LEN; i++)
        mic[i] = x[i] ^ s0[i];
    bool ok = crypt_equal(mic, c + CCMP_HDR_LEN + len, CCMP_MIC_LEN);
    if (!ok) {
        crypt_wipe(out, len);
    } else {
        *out_len = len;
        *pn      = n;
        *key_id  = (uint8_t)(c[3] >> 6);
    }
    crypt_wipe(x, sizeof(x));
    crypt_wipe(s0, sizeof(s0));
    return ok;
}
