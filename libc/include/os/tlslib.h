#ifndef _OS_TLSLIB_H
#define _OS_TLSLIB_H

/*
 * tlslib: TLS 1.0-1.2 client connections (BearSSL 0.6) as a user library
 * (os/ulib.h), with the Mozilla root certificates built in, so programs
 * can talk https without each carrying ~200 KB of crypto.
 *
 * The library file is <home>/sys/lib/tls, built from programs/tlslib. A
 * program loads it with ulib_open() (os/ulib.h) and calls through the
 * table below; wget does (programs/wget/src/tls.c).
 *
 * The library never makes a system call. The program hands it:
 *   - the socket I/O, as two functions (recv / send, like BearSSL's
 *     br_sslio callbacks: > 0 bytes moved, -1 on error or end)
 *   - random bytes for the keys (os_random(), SYS_RANDOM): at least
 *     TLS_SEED_MIN of them
 *   - the time (SYS_CLOCK, ms since 1970), to check certificate dates
 *
 * The server's certificate chain must lead to one of the built-in roots
 * and name `host` (SNI is sent too). Nothing is accepted on trust.
 *
 *     tls_conn_t* t = api->open("example.com", seed, 32, now_ms, my_recv, my_send, &sock, &err);
 *     api->write_all(t, request, n);  api->flush(t);
 *     while ((n = api->read(t, buf, sizeof(buf))) > 0) ...
 *     api->close(t);
 *
 * Changing the table: add at the end and leave TLSLIB_ABI alone.
 */

#include <stddef.h>
#include <stdint.h>

#define TLSLIB_NAME     "tls"
#define TLSLIB_ABI      1
#define TLSLIB_SYSPATH  "lib/tls"       // Under <home>/sys (os_syspath)

#define TLS_SEED_MIN    32              // Random bytes open() needs at least

// The library's error codes are BearSSL's (BR_ERR_*); these few the program
// may want to tell apart
#define TLS_ERR_OK          0
#define TLS_ERR_IO          31          // The connection's own recv / send failed (or it closed)
#define TLS_ERR_NOT_TRUSTED 62          // Certificate not from a known root

typedef struct tls_conn tls_conn_t;

// Socket I/O for the library: move up to `length` bytes; the count, or -1
typedef int (*tls_recv_fn)(void* ctx, unsigned char* data, size_t length);
typedef int (*tls_send_fn)(void* ctx, const unsigned char* data, size_t length);

typedef struct {
    uint32_t size;                      // Bytes of this table the library has

    // A connection to `host`, set up but not talked yet: the handshake runs
    // with the first write / flush / read. NULL with *error (BR_ERR_*, or
    // -1 out of memory) when it can't be.
    tls_conn_t* (*open)(const char* host, const void* seed, size_t seed_length, uint64_t unix_ms,
                        tls_recv_fn recv, tls_send_fn send, void* io_ctx, int* error);

    // All of it, buffered: 0, or -1 (error() says why)
    int  (*write_all)(tls_conn_t* t, const void* data, size_t length);
    // Send what is buffered: 0, or -1
    int  (*flush)(tls_conn_t* t);
    // Up to `size` bytes: the count; 0 when the server closed properly
    // (close_notify); -1 on error (TLS_ERR_IO: the connection ended without one)
    int  (*read)(tls_conn_t* t, void* out, size_t size);
    // Why the last call failed (BR_ERR_*), TLS_ERR_OK when nothing did
    int  (*error)(tls_conn_t* t);
    // Words for an error code ("the certificate isn't from a trusted root")
    const char* (*error_text)(int code);
    // Say goodbye (close_notify, best effort) and free it. NULL is fine.
    void (*close)(tls_conn_t* t);
    // "BearSSL 0.6, 117 roots"
    const char* (*version)(void);
} tlslib_api_t;

#endif
