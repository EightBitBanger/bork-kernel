#ifndef KERNEL_CRYPT_WPA_H
#define KERNEL_CRYPT_WPA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//
// WPA2-Personal key derivation (IEEE 802.11-2020 12.7)
//
//   passphrase + SSID  --PBKDF2-->  PMK (32 bytes, the pairwise master key)
//   PMK + both MACs + both nonces  --PRF-384-->  PTK: KCK | KEK | TK
//
// The KCK signs EAPOL-Key frames (the MIC), the KEK unwraps the group key
// the access point sends, and the TK encrypts unicast data (CCMP).
//

#define WPA_PMK_LEN         32
#define WPA_NONCE_LEN       32
#define WPA_KCK_LEN         16
#define WPA_KEK_LEN         16
#define WPA_TK_LEN          16      // CCMP
#define WPA_PTK_LEN         (WPA_KCK_LEN + WPA_KEK_LEN + WPA_TK_LEN)
#define WPA_MIC_LEN         16
#define WPA_PASSPHRASE_MIN  8
#define WPA_PASSPHRASE_MAX  63      // 64 means hex digits of the PSK itself

typedef struct {
    uint8_t kck[WPA_KCK_LEN];
    uint8_t kek[WPA_KEK_LEN];
    uint8_t tk[WPA_TK_LEN];
} wpa_ptk_t;

// The PMK from a passphrase (8 to 63 printable ASCII characters) and the
// network's name, or from 64 hex digits taken as the PMK itself. False: a
// passphrase of the wrong length or characters. Takes 8192 SHA-1 blocks:
// tens of milliseconds, so not under a spinlock.
bool wpa_pmk(const char* passphrase, size_t len, const uint8_t* ssid, size_t ssid_len,
             uint8_t pmk[WPA_PMK_LEN]);

// PRF-n (12.7.1.2): `out_len` bytes from HMAC-SHA1(key, label 0 data i)
void wpa_prf(const uint8_t* key, size_t key_len, const char* label, const uint8_t* data, size_t data_len,
             uint8_t* out, size_t out_len);

// The PTK for a pairwise exchange: aa is the access point's MAC, spa ours
void wpa_ptk(const uint8_t pmk[WPA_PMK_LEN], const uint8_t aa[6], const uint8_t spa[6],
             const uint8_t anonce[WPA_NONCE_LEN], const uint8_t snonce[WPA_NONCE_LEN], wpa_ptk_t* ptk);

// The MIC of an EAPOL-Key frame (key descriptor version 2: HMAC-SHA1-128).
// `frame` is the whole EAPOL frame with its MIC field zeroed.
void wpa_mic(const uint8_t kck[WPA_KCK_LEN], const uint8_t* frame, size_t len, uint8_t mic[WPA_MIC_LEN]);

#endif
