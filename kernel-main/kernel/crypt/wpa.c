#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/crypt/crypt.h>
#include <kernel/crypt/sha1.h>
#include <kernel/crypt/wpa.h>
#include <kernel/util/string.h>

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool wpa_pmk(const char* passphrase, size_t len, const uint8_t* ssid, size_t ssid_len,
             uint8_t pmk[WPA_PMK_LEN]) {
    if (len == 2 * WPA_PMK_LEN) {
        for (size_t i = 0; i < WPA_PMK_LEN; i++) {
            int hi = hex_digit(passphrase[2 * i]), lo = hex_digit(passphrase[2 * i + 1]);
            if (hi < 0 || lo < 0) {
                crypt_wipe(pmk, WPA_PMK_LEN);
                return false;
            }
            pmk[i] = (uint8_t)(hi << 4 | lo);
        }
        return true;
    }
    if (len < WPA_PASSPHRASE_MIN || len > WPA_PASSPHRASE_MAX || ssid_len > 32)
        return false;
    for (size_t i = 0; i < len; i++) {
        if (passphrase[i] < 0x20 || passphrase[i] > 0x7E)
            return false;               // The standard allows printable ASCII only
    }
    pbkdf2_sha1((const uint8_t*)passphrase, len, ssid, ssid_len, 4096, pmk, WPA_PMK_LEN);
    return true;
}

void wpa_prf(const uint8_t* key, size_t key_len, const char* label, const uint8_t* data, size_t data_len,
             uint8_t* out, size_t out_len) {
    uint8_t zero = 0, counter = 0, block[SHA1_LEN];
    const uint8_t* parts[4] = { (const uint8_t*)label, &zero, data, &counter };
    size_t lens[4] = { strlen(label), 1, data_len, 1 };
    while (out_len > 0) {
        hmac_sha1_vec(key, key_len, 4, parts, lens, block);
        size_t take = out_len < SHA1_LEN ? out_len : SHA1_LEN;
        memcpy(out, block, take);
        out += take;
        out_len -= take;
        counter++;
    }
    crypt_wipe(block, sizeof(block));
}

void wpa_ptk(const uint8_t pmk[WPA_PMK_LEN], const uint8_t aa[6], const uint8_t spa[6],
             const uint8_t anonce[WPA_NONCE_LEN], const uint8_t snonce[WPA_NONCE_LEN], wpa_ptk_t* ptk) {
    // Min(AA, SPA) || Max(AA, SPA) || Min(ANonce, SNonce) || Max(ANonce, SNonce)
    uint8_t data[2 * 6 + 2 * WPA_NONCE_LEN];
    bool aa_first = memcmp(aa, spa, 6) < 0;
    memcpy(data,     aa_first ? aa : spa, 6);
    memcpy(data + 6, aa_first ? spa : aa, 6);
    bool an_first = memcmp(anonce, snonce, WPA_NONCE_LEN) < 0;
    memcpy(data + 12,                 an_first ? anonce : snonce, WPA_NONCE_LEN);
    memcpy(data + 12 + WPA_NONCE_LEN, an_first ? snonce : anonce, WPA_NONCE_LEN);

    uint8_t out[WPA_PTK_LEN];
    wpa_prf(pmk, WPA_PMK_LEN, "Pairwise key expansion", data, sizeof(data), out, sizeof(out));
    memcpy(ptk->kck, out, WPA_KCK_LEN);
    memcpy(ptk->kek, out + WPA_KCK_LEN, WPA_KEK_LEN);
    memcpy(ptk->tk,  out + WPA_KCK_LEN + WPA_KEK_LEN, WPA_TK_LEN);
    crypt_wipe(out, sizeof(out));
}

void wpa_mic(const uint8_t kck[WPA_KCK_LEN], const uint8_t* frame, size_t len, uint8_t mic[WPA_MIC_LEN]) {
    uint8_t full[SHA1_LEN];
    hmac_sha1(kck, WPA_KCK_LEN, frame, len, full);
    memcpy(mic, full, WPA_MIC_LEN);
    crypt_wipe(full, sizeof(full));
}
