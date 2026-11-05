#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/crypt/crypt.h>
#include <kernel/crypt/sha1.h>
#include <kernel/util/string.h>

//
// SHA-1 (FIPS 180-4), HMAC-SHA1 (RFC 2104) and PBKDF2 (RFC 8018)
//
// SHA-1 is broken for collisions, but WPA2 uses it only inside HMAC (PRF,
// MIC, PBKDF2), where that doesn't matter.
//

#define ROL(x, n)   (((x) << (n)) | ((x) >> (32 - (n))))

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void compress(uint32_t h[5], const uint8_t block[SHA1_BLOCK]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = be32(block + 4 * i);
    for (int i = 16; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROL(b, 30);
        b = a;
        a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    crypt_wipe(w, sizeof(w));
}

void sha1_init(sha1_t* s) {
    s->h[0] = 0x67452301u;
    s->h[1] = 0xEFCDAB89u;
    s->h[2] = 0x98BADCFEu;
    s->h[3] = 0x10325476u;
    s->h[4] = 0xC3D2E1F0u;
    s->length = 0;
    s->used   = 0;
}

void sha1_update(sha1_t* s, const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    s->length += n;
    while (n > 0) {
        size_t take = SHA1_BLOCK - s->used;
        if (take > n) take = n;
        memcpy(s->block + s->used, p, take);
        s->used += take;
        p += take;
        n -= take;
        if (s->used == SHA1_BLOCK) {
            compress(s->h, s->block);
            s->used = 0;
        }
    }
}

void sha1_final(sha1_t* s, uint8_t out[SHA1_LEN]) {
    uint64_t bits = s->length * 8;
    uint8_t pad = 0x80;
    sha1_update(s, &pad, 1);
    pad = 0;
    while (s->used != SHA1_BLOCK - 8)
        sha1_update(s, &pad, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++)
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(s, len, 8);
    for (int i = 0; i < 5; i++) {
        out[4 * i]     = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
    crypt_wipe(s, sizeof(*s));
}

void sha1(const void* data, size_t n, uint8_t out[SHA1_LEN]) {
    sha1_t s;
    sha1_init(&s);
    sha1_update(&s, data, n);
    sha1_final(&s, out);
}

void hmac_sha1_vec(const uint8_t* key, size_t key_len, size_t count, const uint8_t* const* data,
                   const size_t* len, uint8_t out[SHA1_LEN]) {
    uint8_t k[SHA1_BLOCK], pad[SHA1_BLOCK], inner[SHA1_LEN];
    memset(k, 0, sizeof(k));
    if (key_len > SHA1_BLOCK)
        sha1(key, key_len, k);          // A long key is hashed first
    else
        memcpy(k, key, key_len);

    sha1_t s;
    for (int i = 0; i < SHA1_BLOCK; i++)
        pad[i] = k[i] ^ 0x36;
    sha1_init(&s);
    sha1_update(&s, pad, SHA1_BLOCK);
    for (size_t i = 0; i < count; i++)
        sha1_update(&s, data[i], len[i]);
    sha1_final(&s, inner);

    for (int i = 0; i < SHA1_BLOCK; i++)
        pad[i] = k[i] ^ 0x5C;
    sha1_init(&s);
    sha1_update(&s, pad, SHA1_BLOCK);
    sha1_update(&s, inner, SHA1_LEN);
    sha1_final(&s, out);

    crypt_wipe(k, sizeof(k));
    crypt_wipe(pad, sizeof(pad));
    crypt_wipe(inner, sizeof(inner));
}

void hmac_sha1(const uint8_t* key, size_t key_len, const void* data, size_t n, uint8_t out[SHA1_LEN]) {
    const uint8_t* d = (const uint8_t*)data;
    hmac_sha1_vec(key, key_len, 1, &d, &n, out);
}

void pbkdf2_sha1(const uint8_t* pass, size_t pass_len, const uint8_t* salt, size_t salt_len,
                 uint32_t iterations, uint8_t* out, size_t out_len) {
    uint8_t u[SHA1_LEN], t[SHA1_LEN];
    for (uint32_t block = 1; out_len > 0; block++) {
        uint8_t index[4] = { (uint8_t)(block >> 24), (uint8_t)(block >> 16), (uint8_t)(block >> 8),
                             (uint8_t)block };
        const uint8_t* parts[2] = { salt, index };
        size_t lens[2] = { salt_len, 4 };
        hmac_sha1_vec(pass, pass_len, 2, parts, lens, u);
        memcpy(t, u, SHA1_LEN);
        for (uint32_t i = 1; i < iterations; i++) {
            hmac_sha1(pass, pass_len, u, SHA1_LEN, u);
            for (int j = 0; j < SHA1_LEN; j++)
                t[j] ^= u[j];
        }
        size_t take = out_len < SHA1_LEN ? out_len : SHA1_LEN;
        memcpy(out, t, take);
        out += take;
        out_len -= take;
    }
    crypt_wipe(u, sizeof(u));
    crypt_wipe(t, sizeof(t));
}

//
// Shared helpers (crypt.h)
//

void crypt_wipe(void* p, size_t n) {
    volatile uint8_t* v = (volatile uint8_t*)p;
    while (n-- > 0)
        *v++ = 0;
}

bool crypt_equal(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++)
        diff |= x[i] ^ y[i];
    return diff == 0;
}
