#ifndef KERNEL_CRYPT_AES_H
#define KERNEL_CRYPT_AES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AES_BLOCK       16
#define AES128_KEY_LEN  16

// An expanded AES-128 key (FIPS 197): 11 round keys
typedef struct {
    uint8_t round[11][AES_BLOCK];
} aes128_t;

void aes128_init(aes128_t* a, const uint8_t key[AES128_KEY_LEN]);
void aes128_encrypt(const aes128_t* a, const uint8_t in[AES_BLOCK], uint8_t out[AES_BLOCK]);
void aes128_decrypt(const aes128_t* a, const uint8_t in[AES_BLOCK], uint8_t out[AES_BLOCK]);

// AES key unwrap (RFC 3394) with a 128-bit KEK: `in_len` bytes (a multiple
// of 8, at least 24) into in_len - 8 bytes of `out`. False when the
// integrity check fails (wrong KEK or damaged data); `out` is cleared then.
bool aes_unwrap(const uint8_t kek[AES128_KEY_LEN], const uint8_t* in, size_t in_len, uint8_t* out);

#endif
