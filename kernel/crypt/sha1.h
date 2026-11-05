#ifndef KERNEL_CRYPT_SHA1_H
#define KERNEL_CRYPT_SHA1_H

#include <stddef.h>
#include <stdint.h>

#define SHA1_LEN        20
#define SHA1_BLOCK      64

typedef struct {
    uint32_t h[5];
    uint64_t length;            // Bytes so far
    uint8_t  block[SHA1_BLOCK];
    size_t   used;              // Bytes waiting in block
} sha1_t;

void sha1_init(sha1_t* s);
void sha1_update(sha1_t* s, const void* data, size_t n);
void sha1_final(sha1_t* s, uint8_t out[SHA1_LEN]);         // Wipes s
void sha1(const void* data, size_t n, uint8_t out[SHA1_LEN]);

// HMAC-SHA1 (RFC 2104) over `count` pieces of data, one after another
void hmac_sha1_vec(const uint8_t* key, size_t key_len, size_t count, const uint8_t* const* data,
                   const size_t* len, uint8_t out[SHA1_LEN]);
void hmac_sha1(const uint8_t* key, size_t key_len, const void* data, size_t n, uint8_t out[SHA1_LEN]);

// PBKDF2 with HMAC-SHA1 (RFC 8018): `out_len` bytes of key
void pbkdf2_sha1(const uint8_t* pass, size_t pass_len, const uint8_t* salt, size_t salt_len,
                 uint32_t iterations, uint8_t* out, size_t out_len);

#endif
