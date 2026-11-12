/*
 * tlslib: TLS client connections with BearSSL 0.6 as a user library
 * (os/ulib.h, os/tlslib.h). Built into <home>/sys/lib/tls by build.sh.
 *
 * BearSSL needs no heap, no system calls and no clock of its own, which
 * suits a library that may make none: one block per connection comes from
 * the program's malloc (ulib_host_t), the socket I/O is the program's two
 * callbacks, and the seed and the time come in with open().
 *
 * Built as configured in build.sh: no AES-NI / SSE2 / RDRAND code (the
 * kernel doesn't promise SSE state is kept, and the program brings its own
 * randomness), no /dev/urandom or time() (there are none).
 *
 * The trust anchors are the Mozilla roots in roots.c (gen-roots.sh).
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <os/tlslib.h>
#include <os/ulib.h>
#include "bearssl.h"

// roots.c
extern const br_x509_trust_anchor TAs[];
extern const size_t TAs_NUM_VALUE;

static const ulib_host_t* host;

struct tls_conn {
    br_ssl_client_context  cc;
    br_x509_minimal_context xc;
    br_sslio_context       io;
    tls_send_fn            send;
    void*                  io_ctx;
    int                    last;            // Our own error, when BearSSL has none
    unsigned char          iobuf[BR_SSL_BUFSIZE_BIDI];
};

// n / d for a 64-bit n and a 32-bit d, without libgcc's __udivdi3 (a
// library links nothing but ulib/runtime.c and math.c)
static uint64_t div64(uint64_t n, uint32_t d, uint32_t* rem) {
    uint64_t q = 0, r = 0;
    for (int bit = 63; bit >= 0; bit--) {
        r = (r << 1) | ((n >> bit) & 1u);
        if (r >= d) {
            r -= d;
            q |= (uint64_t)1 << bit;
        }
    }
    if (rem != NULL)
        *rem = (uint32_t)r;
    return q;
}

// Days from 0000-01-01 (proleptic Gregorian, as br_x509_minimal_set_time
// counts them) to 1970-01-01
#define DAYS_TO_1970    719528u

static tls_conn_t* tls_open(const char* hostname, const void* seed, size_t seed_length, uint64_t unix_ms,
                            tls_recv_fn recv, tls_send_fn send, void* io_ctx, int* error) {
    int dummy;
    if (error == NULL)
        error = &dummy;
    *error = BR_ERR_BAD_PARAM;
    if (hostname == NULL || seed == NULL || seed_length < TLS_SEED_MIN || recv == NULL || send == NULL)
        return NULL;

    tls_conn_t* t = (tls_conn_t*)host->malloc((uint32_t)sizeof(tls_conn_t));
    if (t == NULL) {
        *error = -1;
        return NULL;
    }
    memset(t, 0, sizeof(*t));
    t->send = send;
    t->io_ctx = io_ctx;

    // Every suite and key type BearSSL has, checked against the roots
    br_ssl_client_init_full(&t->cc, &t->xc, TAs, TAs_NUM_VALUE);
    uint32_t day_secs;
    uint64_t days = div64(div64(unix_ms, 1000u, NULL), 86400u, &day_secs);
    br_x509_minimal_set_time(&t->xc, (uint32_t)(DAYS_TO_1970 + days), day_secs);
    br_ssl_engine_set_buffer(&t->cc.eng, t->iobuf, sizeof(t->iobuf), 1);
    br_ssl_engine_inject_entropy(&t->cc.eng, seed, seed_length);
    if (!br_ssl_client_reset(&t->cc, hostname, 0)) {
        *error = br_ssl_engine_last_error(&t->cc.eng);
        if (*error == BR_ERR_OK)
            *error = BR_ERR_BAD_STATE;
        memset(t, 0, sizeof(*t));
        host->free(t);
        return NULL;
    }
    br_sslio_init(&t->io, &t->cc.eng, recv, io_ctx, send, io_ctx);
    *error = BR_ERR_OK;
    return t;
}

static int tls_write_all(tls_conn_t* t, const void* data, size_t length) {
    return br_sslio_write_all(&t->io, data, length) == 0 ? 0 : -1;
}

static int tls_flush(tls_conn_t* t) {
    return br_sslio_flush(&t->io) == 0 ? 0 : -1;
}

static int tls_read(tls_conn_t* t, void* out, size_t size) {
    if (size == 0)
        return 0;
    int n = br_sslio_read(&t->io, out, size);
    if (n > 0)
        return n;
    // -1: closed. Properly (the server's close_notify) leaves no error.
    return br_ssl_engine_last_error(&t->cc.eng) == BR_ERR_OK ? 0 : -1;
}

static int tls_error(tls_conn_t* t) {
    if (t == NULL)
        return BR_ERR_BAD_PARAM;
    int e = br_ssl_engine_last_error(&t->cc.eng);
    return e != BR_ERR_OK ? e : t->last;
}

static const char* tls_error_text(int code) {
    if (code == -1)
        return "out of memory";
    if (code & BR_ERR_RECV_FATAL_ALERT) {
        switch (code & 0xFF) {             // The server's alert
            case 70:  return "the server only speaks a newer TLS than this one has (1.3)";
            case 40:
            case 71:  return "the server and this TLS have no cipher suite or version in common";
            case 112: return "the server doesn't know that host name";
            default:  return "the server ended the handshake (alert)";
        }
    }
    if (code & BR_ERR_SEND_FATAL_ALERT)
        code &= ~BR_ERR_SEND_FATAL_ALERT;
    switch (code) {
        case BR_ERR_OK:                     return "no error";
        case BR_ERR_UNSUPPORTED_VERSION:
        case BR_ERR_BAD_VERSION:            return "the server wants a TLS version this one doesn't have (only 1.0 - 1.2)";
        case BR_ERR_BAD_MAC:                return "a record was tampered with or damaged";
        case BR_ERR_NO_RANDOM:              return "no random seed";
        case BR_ERR_BAD_CIPHER_SUITE:       return "no cipher suite in common with the server";
        case BR_ERR_BAD_SIGNATURE:          return "the server's signature doesn't check out";
        case BR_ERR_IO:                     return "the connection ended";
        case BR_ERR_X509_EXPIRED:           return "the certificate has expired, or isn't valid yet (is the clock right?)";
        case BR_ERR_X509_TIME_UNKNOWN:      return "no time to check the certificate against";
        case BR_ERR_X509_BAD_SERVER_NAME:   return "the certificate is for another name";
        case BR_ERR_X509_NOT_TRUSTED:       return "the certificate isn't from a trusted root";
        case BR_ERR_X509_EMPTY_CHAIN:       return "the server sent no certificate";
        case BR_ERR_X509_UNSUPPORTED:
        case BR_ERR_X509_WRONG_KEY_TYPE:    return "the certificate uses something this TLS doesn't support";
        case BR_ERR_X509_BAD_SIGNATURE:     return "a certificate's signature is wrong";
        case BR_ERR_X509_NOT_CA:            return "a certificate in the chain isn't a CA";
        case BR_ERR_X509_WEAK_PUBLIC_KEY:   return "the certificate's key is too weak";
        case BR_ERR_X509_LIMIT_EXCEEDED:    return "the certificate chain is too long";
        default:
            if (code >= BR_ERR_X509_OK && code < 64)
                return "the certificate couldn't be checked";
            return "TLS handshake or record error";
    }
}

static void tls_close(tls_conn_t* t) {
    if (t == NULL)
        return;
    // close_notify, without waiting for the server's answer
    br_ssl_engine_context* eng = &t->cc.eng;
    br_ssl_engine_close(eng);
    for (int rounds = 0; rounds < 4 && (br_ssl_engine_current_state(eng) & BR_SSL_SENDREC); rounds++) {
        size_t len;
        unsigned char* buf = br_ssl_engine_sendrec_buf(eng, &len);
        int n = t->send(t->io_ctx, buf, len);
        if (n <= 0)
            break;
        br_ssl_engine_sendrec_ack(eng, (size_t)n);
    }
    memset(t, 0, sizeof(*t));               // Keys gone before the memory is reused
    host->free(t);
}

static const char* tls_version(void) {
    return "BearSSL 0.6, Mozilla roots";
}

static const tlslib_api_t api = {
    .size       = sizeof(tlslib_api_t),
    .open       = tls_open,
    .write_all  = tls_write_all,
    .flush      = tls_flush,
    .read       = tls_read,
    .error      = tls_error,
    .error_text = tls_error_text,
    .close      = tls_close,
    .version    = tls_version,
};

__attribute__((section(".text.ulib_entry")))
const void* ulib_entry(const ulib_host_t* h) {
    if (h == NULL || h->size < offsetof(ulib_host_t, free) + sizeof(h->free) ||
        h->malloc == NULL || h->free == NULL)
        return NULL;
    host = h;
    return &api;
}
