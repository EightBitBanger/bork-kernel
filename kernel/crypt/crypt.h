#ifndef KERNEL_CRYPT_H
#define KERNEL_CRYPT_H

//
// Cryptography for the kernel (kernel/crypt)
//
//   sha1.c     SHA-1, HMAC-SHA1, PBKDF2-HMAC-SHA1
//   aes.c      AES-128 (one block at a time) and AES key unwrap (RFC 3394)
//   ccmp.c     CCMP: AES-CCM as 802.11 uses it to encrypt data frames
//   wpa.c      WPA2-Personal keys: the PSK from a passphrase, the PTK, the
//              EAPOL-Key MIC
//   random.c   Random bytes for nonces, from a SHA-1 pool fed by the caller
//
// Everything here is plain C on byte arrays, with no allocation and no
// locks: callers keep their own state. Comparisons of secrets go through
// crypt_equal (constant time) and secrets are cleared with crypt_wipe.
//
// Checked against the standards' test vectors by tests/crypt (a host
// program, outside kernel/ so it isn't built into the kernel).
//

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Clear memory that held a secret (not optimised away)
void crypt_wipe(void* p, size_t n);

// a == b over n bytes, taking the same time wherever they differ
bool crypt_equal(const void* a, const void* b, size_t n);

#endif
