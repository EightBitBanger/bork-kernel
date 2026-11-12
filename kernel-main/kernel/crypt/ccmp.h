#ifndef KERNEL_CRYPT_CCMP_H
#define KERNEL_CRYPT_CCMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kernel/crypt/aes.h>

//
// CCMP (IEEE 802.11-2020 12.5.3): how WPA2 encrypts data frames. AES-CCM
// with a 128-bit temporal key (TK), an 8-byte MIC, and a 48-bit packet
// number (PN) that must go up with every frame sent under a key.
//
// A protected frame is: the 802.11 header (`hdr_len` bytes, as sent), the
// 8-byte CCMP header (PN and key id), the encrypted body, the 8-byte MIC.
// The header's Protected bit must already be set.
//

#define CCMP_HDR_LEN    8
#define CCMP_MIC_LEN    8
#define CCMP_PN_MAX     0xFFFFFFFFFFFFull

// Encrypt `len` bytes of `body` for the frame whose header is `hdr`. Writes
// the CCMP header, the encrypted body and the MIC to `out` (len + 16
// bytes). `out` may overlap `body` (encrypting in place: out = body - 8).
// False: bad header, or the PN is past CCMP_PN_MAX.
bool ccmp_encrypt(const aes128_t* tk, const uint8_t* hdr, size_t hdr_len, uint64_t pn, uint8_t key_id,
                  const uint8_t* body, size_t len, uint8_t* out);

// Check and decrypt a received frame of `frame_len` bytes whose header is
// `hdr_len` long. The body (frame_len - hdr_len - 16 bytes) goes to `out`,
// its length to *out_len, the frame's PN to *pn and its key id to *key_id.
// False: too short, no ExtIV bit, or the MIC is wrong (`out` cleared). The
// caller checks the PN went up (replays). `out` may be where the encrypted
// body is (in place).
bool ccmp_decrypt(const aes128_t* tk, const uint8_t* frame, size_t frame_len, size_t hdr_len, uint8_t* out,
                  size_t* out_len, uint64_t* pn, uint8_t* key_id);

#endif
