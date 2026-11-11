#ifndef KERNEL_CRYPT_TKIP_H
#define KERNEL_CRYPT_TKIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//
// TKIP (IEEE 802.11-2020 12.5.2), receive side only: what a WPA2 station
// needs on a "WPA/WPA2 mixed" network, where the access point encrypts its
// broadcasts and multicasts (the group key) with TKIP while our own
// traffic (the pairwise key) stays CCMP. Nothing is ever sent with TKIP.
//
// A TKIP frame is: the 802.11 header (`hdr_len` bytes), the 8-byte IV /
// extended IV (TSC1, WEP seed, TSC0, key id | ExtIV, TSC2..TSC5), then,
// RC4 encrypted, the MSDU, its 8-byte Michael MIC and a 4-byte CRC-32 ICV.
// The RC4 key comes from the temporal key, the transmitter's address and
// the 48-bit TSC (the key mixing, phases 1 and 2).
//
// TKIP is weak by today's standards (it is why WPA2-AES exists). The
// Michael MIC is checked, the TSC must go up, and the caller only takes
// group-addressed frames from the access point with it.
//

#define TKIP_IV_LEN     8
#define TKIP_MIC_LEN    8
#define TKIP_ICV_LEN    4
#define TKIP_TK_LEN     16
#define TKIP_GTK_LEN    32          // TK, authenticator's Tx MIC key, its Rx MIC key

typedef struct {
    uint8_t tk[TKIP_TK_LEN];
    uint8_t rx_mic[8];              // The Michael key for frames we receive
} tkip_key_t;

// A group key as the 4-way or group handshake delivers it (32 bytes). We
// receive with the authenticator's Tx MIC key (bytes 16..23).
void tkip_set_group_key(tkip_key_t* key, const uint8_t gtk[TKIP_GTK_LEN]);

// Check and decrypt a received frame of `frame_len` bytes whose header is
// `hdr_len` long (Protected bit set). The MSDU goes to `out` (frame_len -
// hdr_len - 20 bytes at most), its length to *out_len, the TSC to *tsc and
// the key id to *key_id. `da` and `sa` are the frame's destination and
// source (for the MIC), `priority` its TID (0 for non-QoS data).
// False: too short, no ExtIV bit, a bad ICV or a bad Michael MIC (`out`
// cleared). The caller checks the TSC went up. `out` may be where the
// encrypted body is (in place).
bool tkip_decrypt(const tkip_key_t* key, const uint8_t* frame, size_t frame_len, size_t hdr_len,
                  const uint8_t* da, const uint8_t* sa, uint8_t priority,
                  uint8_t* out, size_t* out_len, uint64_t* tsc, uint8_t* key_id);

// The pieces, for the test vectors
void tkip_mix(const uint8_t tk[TKIP_TK_LEN], const uint8_t ta[6], uint64_t tsc, uint8_t rc4_key[16]);
void tkip_michael(const uint8_t key[8], const uint8_t* data, size_t len, uint8_t mic[8]);
void tkip_rc4(const uint8_t key[16], const uint8_t* in, size_t len, uint8_t* out);
uint32_t tkip_crc32(const uint8_t* data, size_t len);

#endif
