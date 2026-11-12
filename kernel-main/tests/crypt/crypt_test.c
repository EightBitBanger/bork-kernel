// Host test for kernel/crypt: the standards' test vectors, plus CCMP and
// key unwrap round trips and tampering.
//
//   make            (builds and runs it)

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kernel/crypt/aes.h>
#include <kernel/crypt/ccmp.h>
#include <kernel/crypt/tkip.h>
#include <kernel/crypt/crypt.h>
#include <kernel/crypt/random.h>
#include <kernel/crypt/sha1.h>
#include <kernel/crypt/wpa.h>

static int checks, failures;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static size_t unhex(const char* s, uint8_t* out) {
    size_t n = 0;
    while (s[0] && s[1]) {
        if (s[0] == ' ') { s++; continue; }
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static bool same(const uint8_t* got, const char* want_hex) {
    uint8_t want[512];
    size_t n = unhex(want_hex, want);
    if (memcmp(got, want, n) == 0)
        return true;
    printf("  got  ");
    for (size_t i = 0; i < n; i++) printf("%02x", got[i]);
    printf("\n  want %s\n", want_hex);
    return false;
}

static void test_sha1(void) {
    uint8_t d[SHA1_LEN];
    sha1("abc", 3, d);
    CHECK(same(d, "a9993e364706816aba3e25717850c26c9cd0d89d"));
    sha1("", 0, d);
    CHECK(same(d, "da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha1(two, strlen(two), d);
    CHECK(same(d, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));

    // A million 'a's, fed in odd pieces
    sha1_t s;
    sha1_init(&s);
    char a[997];
    memset(a, 'a', sizeof(a));
    size_t left = 1000000;
    while (left > 0) {
        size_t n = left < sizeof(a) ? left : sizeof(a);
        sha1_update(&s, a, n);
        left -= n;
    }
    sha1_final(&s, d);
    CHECK(same(d, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
}

static void test_hmac(void) {
    uint8_t key[80], d[SHA1_LEN];
    // RFC 2202 cases 1, 2 and 6 (a key longer than a block)
    memset(key, 0x0b, 20);
    hmac_sha1(key, 20, "Hi There", 8, d);
    CHECK(same(d, "b617318655057264e28bc0b6fb378c8ef146be00"));
    hmac_sha1((const uint8_t*)"Jefe", 4, "what do ya want for nothing?", 28, d);
    CHECK(same(d, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"));
    memset(key, 0xaa, 80);
    const char* m = "Test Using Larger Than Block-Size Key - Hash Key First";
    hmac_sha1(key, 80, m, strlen(m), d);
    CHECK(same(d, "aa4ae5e15272d00e95705637ce8a3b55ed402112"));
}

static void test_pbkdf2(void) {
    // RFC 6070
    uint8_t d[25];
    pbkdf2_sha1((const uint8_t*)"password", 8, (const uint8_t*)"salt", 4, 1, d, 20);
    CHECK(same(d, "0c60c80f961f0e71f3a9b524af6012062fe037a6"));
    pbkdf2_sha1((const uint8_t*)"password", 8, (const uint8_t*)"salt", 4, 4096, d, 20);
    CHECK(same(d, "4b007901b765489abead49d926f721d065a429c1"));
    pbkdf2_sha1((const uint8_t*)"passwordPASSWORDpassword", 24,
                (const uint8_t*)"saltSALTsaltSALTsaltSALTsaltSALTsalt", 36, 4096, d, 25);
    CHECK(same(d, "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038"));
}

static void test_wpa(void) {
    // IEEE 802.11-2020 J.4: passphrase to PSK
    uint8_t pmk[WPA_PMK_LEN];
    CHECK(wpa_pmk("password", 8, (const uint8_t*)"IEEE", 4, pmk));
    CHECK(same(pmk, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"));
    CHECK(wpa_pmk("ThisIsAPassword", 15, (const uint8_t*)"ThisIsASSID", 11, pmk));
    CHECK(same(pmk, "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af"));
    CHECK(wpa_pmk("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 64,
                  (const uint8_t*)"x", 1, pmk) && pmk[0] == 0xaa && pmk[31] == 0xaa);
    CHECK(!wpa_pmk("short", 5, (const uint8_t*)"x", 1, pmk));
    CHECK(!wpa_pmk("tab\there!", 9, (const uint8_t*)"x", 1, pmk));
    CHECK(!wpa_pmk("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", 64,
                   (const uint8_t*)"x", 1, pmk));

    // J.3: PRF-512
    uint8_t key[20], out[64];
    memset(key, 0x0b, 20);
    wpa_prf(key, 20, "prefix", (const uint8_t*)"Hi There", 8, out, 64);
    CHECK(same(out, "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee"
                    "75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a"));

    // The PTK, against an independent implementation (Python's hmac)
    uint8_t aa[6], spa[6], an[32], sn[32];
    unhex("a0a1a2a3a4a5", aa);
    unhex("b0b1b2b3b4b5", spa);
    for (int i = 0; i < 32; i++) { an[i] = (uint8_t)(0x20 + i); sn[i] = (uint8_t)(0x40 + i); }
    wpa_pmk("ThisIsAPassword", 15, (const uint8_t*)"ThisIsASSID", 11, pmk);
    wpa_ptk_t ptk, ptk2;
    wpa_ptk(pmk, aa, spa, an, sn, &ptk);
    CHECK(same(ptk.kck, "24a1d218fd74b1b185e5ee27b6ae2743"));
    CHECK(same(ptk.kek, "5fc0160d5320051f85fdf92a8317adcc"));
    CHECK(same(ptk.tk,  "ccf7ea2f02701d19d546522f1b69de03"));
    wpa_ptk(pmk, spa, aa, sn, an, &ptk2);          // Order doesn't matter
    CHECK(memcmp(&ptk, &ptk2, sizeof(ptk)) == 0);

    uint8_t mic[WPA_MIC_LEN], full[SHA1_LEN];
    wpa_mic(ptk.kck, (const uint8_t*)"frame", 5, mic);
    hmac_sha1(ptk.kck, 16, "frame", 5, full);
    CHECK(memcmp(mic, full, 16) == 0);
}

static void test_aes(void) {
    // FIPS 197 C.1
    uint8_t key[16], pt[16], ct[16], back[16];
    unhex("000102030405060708090a0b0c0d0e0f", key);
    unhex("00112233445566778899aabbccddeeff", pt);
    aes128_t a;
    aes128_init(&a, key);
    aes128_encrypt(&a, pt, ct);
    CHECK(same(ct, "69c4e0d86a7b0430d8cdb78070b4c55a"));
    aes128_decrypt(&a, ct, back);
    CHECK(memcmp(back, pt, 16) == 0);
    // FIPS 197 appendix B
    unhex("2b7e151628aed2a6abf7158809cf4f3c", key);
    unhex("3243f6a8885a308d313198a2e0370734", pt);
    aes128_init(&a, key);
    aes128_encrypt(&a, pt, ct);
    CHECK(same(ct, "3925841d02dc09fbdc118597196a0b32"));

    // RFC 3394 4.1: unwrap, and a damaged one
    uint8_t kek[16], wrapped[24], k[16];
    unhex("000102030405060708090a0b0c0d0e0f", kek);
    unhex("1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5", wrapped);
    CHECK(aes_unwrap(kek, wrapped, 24, k));
    CHECK(same(k, "00112233445566778899aabbccddeeff"));
    wrapped[10] ^= 1;
    memset(k, 0x55, 16);
    CHECK(!aes_unwrap(kek, wrapped, 24, k) && k[0] == 0 && k[15] == 0);
    CHECK(!aes_unwrap(kek, wrapped, 20, k));
    // Longer key data (a GTK arrives wrapped with its KDE around it), as
    // wrapped by Python's cryptography package:
    uint8_t w40[40], k32[32];
    unhex("11826840774d993ff9c2fa02cca3cea0e93b1e1cf96361f9"
          "3ea6dc2f345194e7b30f964c79f9e61d", w40);
    CHECK(aes_unwrap(kek, w40, 40, k32));
    CHECK(same(k32, "00112233445566778899aabbccddeeff000102030405060708090a0b0c0d0e0f"));
}

static void test_ccmp(void) {
    // IEEE 802.11-2020 J.6.4
    uint8_t tk[16], hdr[24], body[20], frame[24 + 20 + 16], out[64];
    unhex("c97c1f67ce371185514a8a19f2bdd52f", tk);
    unhex("08 48 c3 2c 0f d2 e1 28 a5 7c 50 30 f1 84 44 08 ab ae a5 b8 fc ba 80 33", hdr);
    unhex("f8 ba 1a 55 d0 2f 85 ae 96 7b b6 2f b6 cd a8 eb 7e 78 a0 50", body);
    aes128_t a;
    aes128_init(&a, tk);
    memcpy(frame, hdr, 24);
    CHECK(ccmp_encrypt(&a, hdr, 24, 0xB5039776E70Cull, 0, body, 20, frame + 24));
    CHECK(same(frame + 24, "0c e7 00 20 76 97 03 b5 f3 d0 a2 fe 9a 3d bf 23 42 a6 43 e4 32 46 e8 0c"
                           "3c 04 d0 19 78 45 ce 0b 16 f9 76 23"));

    size_t len;
    uint64_t pn;
    uint8_t key_id = 9;
    CHECK(ccmp_decrypt(&a, frame, sizeof(frame), 24, out, &len, &pn, &key_id));
    CHECK(len == 20 && memcmp(out, body, 20) == 0 && pn == 0xB5039776E70Cull && key_id == 0);

    // Any flipped bit is caught, except in what the AAD leaves out on
    // purpose (a data frame's subtype bits 4-6, frame control byte 1's
    // retry / power / more-data bits, the duration, the sequence number)
    // and the CCMP header's reserved and key id byte
    int tried = 0, caught = 0;
    for (size_t byte = 0; byte < sizeof(frame); byte++) {
        if (byte == 1 || byte == 2 || byte == 3 || byte == 22 || byte == 23 || byte == 26 || byte == 27)
            continue;
        for (int bit = 0; bit < 8; bit++) {
            if (byte == 0 && bit >= 4 && bit <= 6)
                continue;
            frame[byte] ^= (uint8_t)(1 << bit);
            tried++;
            if (!ccmp_decrypt(&a, frame, sizeof(frame), 24, out, &len, &pn, &key_id))
                caught++;
            frame[byte] ^= (uint8_t)(1 << bit);
        }
    }
    CHECK(tried > 400 && caught == tried);

    // A QoS data frame with four addresses, every length up to a few blocks,
    // encrypted in place
    uint8_t q[32 + 8 + 100 + 8];
    for (size_t n = 0; n <= 100; n++) {
        memset(q, 0, sizeof(q));
        q[0] = 0x88;                // QoS data
        q[1] = 0x43;                // To and from DS, protected
        for (int i = 4; i < 30; i++) q[i] = (uint8_t)i;
        q[30] = 5;                  // TID 5
        for (size_t i = 0; i < n; i++) q[40 + i] = (uint8_t)(i * 7);
        CHECK(ccmp_encrypt(&a, q, 32, 1000 + n, 1, q + 40, n, q + 32));
        bool ok = ccmp_decrypt(&a, q, 32 + 16 + n, 32, q + 40, &len, &pn, &key_id);
        bool right = ok && len == n && pn == 1000 + n && key_id == 1;
        for (size_t i = 0; right && i < n; i++)
            right = q[40 + i] == (uint8_t)(i * 7);
        if (!right) { CHECK(right); break; }
    }

    // Too short, no ExtIV, a PN out of range
    CHECK(!ccmp_decrypt(&a, frame, 24 + 15, 24, out, &len, &pn, &key_id));
    frame[27] &= (uint8_t)~0x20;
    CHECK(!ccmp_decrypt(&a, frame, sizeof(frame), 24, out, &len, &pn, &key_id));
    CHECK(!ccmp_encrypt(&a, hdr, 24, CCMP_PN_MAX + 1, 0, body, 20, out));
    CHECK(!ccmp_encrypt(&a, hdr, 20, 1, 0, body, 20, out));
}

// TKIP (receive only): IEEE 802.11 key mixing and Michael vectors, then a
// whole group frame built by an independent implementation
static void test_tkip(void) {
    uint8_t tk[16], ta[6], key[16], mic[8], k8[8];
    for (int i = 0; i < 16; i++) tk[i] = (uint8_t)i;
    unhex("102233445566", ta);
    tkip_mix(tk, ta, 0, key);
    CHECK(same(key, "00200033ea8d2f60ca6d1374234a660b"));
    tkip_mix(tk, ta, 1, key);
    CHECK(same(key, "00200190ffdc314389a9d9d074fd20aa"));
    tkip_mix(tk, ta, 0x123456789abcull, key);
    CHECK(same(key, "9a3abcd9174c532e6aa7c20ddb11b354"));

    static const char* const msgs[] = { "", "M", "Mi", "Mic", "Mich", "Michael" };
    static const char* const want[] = { "82925c1ca1d130b8", "434721ca40639b3f", "e8f9becae97e5d29",
                                        "90038fc6cf13c1db", "d55e100510128986", "0a942b124ecaa546" };
    memset(k8, 0, sizeof(k8));
    for (int i = 0; i < 6; i++) {
        tkip_michael(k8, (const uint8_t*)msgs[i], strlen(msgs[i]), mic);
        CHECK(same(mic, want[i]));
        memcpy(k8, mic, 8);
    }
    CHECK(tkip_crc32((const uint8_t*)"123456789", 9) == 0xcbf43926u);

    uint8_t gtk[32], frame[128], out[128];
    unhex("303132333435363738393a3b3c3d3e3fa1a2a3a4a5a6a7a8b1b2b3b4b5b6b7b8", gtk);
    size_t flen = unhex("08420000ffffffffffff02aabbccddee0e11223344553001567678603412000086f82839ddafd5c9841cafbcad3578a5ff8619c8a0f41fc0269c044c84c82a3ed2eff6f249c2a004f32d02edb31c9c8c", frame);
    tkip_key_t g;
    tkip_set_group_key(&g, gtk);
    size_t len = 0;
    uint64_t tsc = 0;
    uint8_t key_id = 9;
    CHECK(tkip_decrypt(&g, frame, flen, 24, frame + 4, frame + 16, 0, out, &len, &tsc, &key_id));
    CHECK(len == 36 && same(out, "aaaa030000000806000102030405060708090a0b0c0d0e0f101112131415161718191a1b"));
    CHECK(tsc == 0x12345678ull && key_id == 1);

    // In place, as the station does it
    uint8_t copy[128];
    memcpy(copy, frame, flen);
    CHECK(tkip_decrypt(&g, copy, flen, 24, copy + 4, copy + 16, 0, copy + 32, &len, &tsc, &key_id));
    CHECK(len == 36 && memcmp(copy + 32, out, 36) == 0);

    // Every bit flipped after the IV is caught (ICV or MIC)
    int caught = 0;
    for (size_t bit = 32 * 8; bit < flen * 8; bit++) {
        memcpy(copy, frame, flen);
        copy[bit / 8] ^= (uint8_t)(1 << (bit % 8));
        caught += !tkip_decrypt(&g, copy, flen, 24, copy + 4, copy + 16, 0, out, &len, &tsc, &key_id);
    }
    CHECK(caught == (int)((flen - 32) * 8));
    // The wrong MIC key, priority or source: refused
    tkip_key_t bad = g;
    bad.rx_mic[0] ^= 1;
    CHECK(!tkip_decrypt(&bad, frame, flen, 24, frame + 4, frame + 16, 0, out, &len, &tsc, &key_id));
    CHECK(!tkip_decrypt(&g, frame, flen, 24, frame + 4, frame + 16, 3, out, &len, &tsc, &key_id));
    CHECK(!tkip_decrypt(&g, frame, flen, 24, frame + 4, frame + 10, 0, out, &len, &tsc, &key_id));
    // No ExtIV, a bad WEP seed, a fragment, too short
    memcpy(copy, frame, flen); copy[27] &= (uint8_t)~0x20;
    CHECK(!tkip_decrypt(&g, copy, flen, 24, copy + 4, copy + 16, 0, out, &len, &tsc, &key_id));
    memcpy(copy, frame, flen); copy[25] ^= 1;
    CHECK(!tkip_decrypt(&g, copy, flen, 24, copy + 4, copy + 16, 0, out, &len, &tsc, &key_id));
    memcpy(copy, frame, flen); copy[1] |= 0x04;
    CHECK(!tkip_decrypt(&g, copy, flen, 24, copy + 4, copy + 16, 0, out, &len, &tsc, &key_id));
    memcpy(copy, frame, flen); copy[22] |= 0x01;
    CHECK(!tkip_decrypt(&g, copy, flen, 24, copy + 4, copy + 16, 0, out, &len, &tsc, &key_id));
    CHECK(!tkip_decrypt(&g, frame, 24 + 19, 24, frame + 4, frame + 16, 0, out, &len, &tsc, &key_id));
}

static void test_random(void) {
    uint8_t a[48], b[48];
    crypt_random_add("seed", 4);
    crypt_random_bytes(a, sizeof(a));
    crypt_random_bytes(b, sizeof(b));
    CHECK(memcmp(a, b, sizeof(a)) != 0);
    int zeros = 0;
    for (size_t i = 0; i < sizeof(a); i++) zeros += a[i] == 0;
    CHECK(zeros < 6);
    CHECK(crypt_equal(a, a, 48) && !crypt_equal(a, b, 48));
}

int main(void) {
    test_sha1();
    test_hmac();
    test_pbkdf2();
    test_wpa();
    test_aes();
    test_ccmp();
    test_tkip();
    test_random();
    printf("crypt: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
