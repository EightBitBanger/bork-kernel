/*
 * https for wget (tls.h): the tls library (os/tlslib.h) on a TCP socket.
 *
 * The library makes no system calls, so this file brings what it needs:
 * the socket reads and writes (with wget's idle timeout), 32 random bytes
 * from the kernel (os_random, SYS_RANDOM) for the keys, and the wall clock
 * (os_clock_ms, SYS_CLOCK) to check the certificate's dates.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/net.h>
#include <os/os.h>
#include <os/tlslib.h>
#include <os/ulib.h>
#include "tls.h"

struct wget_tls {
    tls_conn_t* conn;
    int         socket;
    int32_t     timeout_ms;
    bool        timed_out;              // The last recv waited too long
};

static const tlslib_api_t* api;
static const char*         load_error;

// The library, loaded once
static const tlslib_api_t* tls_api(void) {
    if (api != NULL || load_error != NULL)
        return api;
    static char why[160];
    char path[256];
    if (os_syspath(TLSLIB_SYSPATH, path, sizeof(path)) == NULL) {
        load_error = "https needs the tls library (sys/" TLSLIB_SYSPATH "), and there's no home folder";
        return NULL;
    }
    ulib_t* lib = ulib_open(path, TLSLIB_NAME, TLSLIB_ABI);
    if (lib == NULL) {
        snprintf(why, sizeof(why), "https needs the tls library %s: %s", path, ulib_error());
        load_error = why;
        return NULL;
    }
    const tlslib_api_t* t = (const tlslib_api_t*)ulib_exports(lib);
    if (t == NULL || t->size < offsetof(tlslib_api_t, version) + sizeof(t->version)) {
        snprintf(why, sizeof(why), "the tls library %s is too old for this wget", path);
        load_error = why;
        ulib_close(lib);
        return NULL;
    }
    api = t;                            // Stays loaded until wget ends
    return api;
}

static int sock_recv(void* ctx, unsigned char* data, size_t length) {
    wget_tls_t* t = (wget_tls_t*)ctx;
    ssize_t n = net_recv_wait(t->socket, data, length, t->timeout_ms);
    if (n > 0)
        return (int)n;
    if (n < 0 && errno == ETIMEDOUT)
        t->timed_out = true;
    return -1;                          // Closed, or an error: the library stops
}

static int sock_send(void* ctx, const unsigned char* data, size_t length) {
    wget_tls_t* t = (wget_tls_t*)ctx;
    return net_send_all(t->socket, data, length) < 0 ? -1 : (int)length;
}

wget_tls_t* wget_tls_open(int socket, const char* host, int32_t timeout_ms, const char** why) {
    const tlslib_api_t* a = tls_api();
    if (a == NULL) {
        *why = load_error;
        return NULL;
    }
    uint64_t now;
    if (os_clock_ms(&now) < 0) {
        *why = "https needs the date to check certificates, and this machine has no clock";
        return NULL;
    }
    unsigned char seed[TLS_SEED_MIN];
    if (os_random(seed, sizeof(seed)) < 0) {
        *why = "no random numbers from the kernel for the keys";
        return NULL;
    }
    wget_tls_t* t = (wget_tls_t*)calloc(1, sizeof(*t));
    if (t == NULL) {
        *why = "out of memory";
        return NULL;
    }
    t->socket = socket;
    t->timeout_ms = timeout_ms;
    int error = 0;
    t->conn = a->open(host, seed, sizeof(seed), now, sock_recv, sock_send, t, &error);
    memset(seed, 0, sizeof(seed));
    if (t->conn == NULL) {
        *why = a->error_text(error);
        free(t);
        return NULL;
    }
    return t;
}

static const char* failure(wget_tls_t* t) {
    int e = api->error(t->conn);
    if (t->timed_out)
        return "the server stopped answering";
    return api->error_text(e);
}

int wget_tls_send(wget_tls_t* t, const void* data, size_t length, const char** why) {
    if (api->write_all(t->conn, data, length) < 0 || api->flush(t->conn) < 0) {
        *why = failure(t);
        return -1;
    }
    return 0;
}

int wget_tls_recv(wget_tls_t* t, void* out, size_t size, bool* timed_out, const char** why) {
    t->timed_out = false;
    int n = api->read(t->conn, out, size);
    *timed_out = t->timed_out;
    if (n >= 0)
        return n;
    // A server that just closes the TCP connection (no close_notify), as
    // many do once the body is out: the end, as for plain HTTP. A body with
    // a length or chunks still finds out it was cut short.
    if (!t->timed_out && api->error(t->conn) == TLS_ERR_IO)
        return 0;
    *why = failure(t);
    return -1;
}

void wget_tls_close(wget_tls_t* t) {
    if (t == NULL)
        return;
    api->close(t->conn);
    free(t);
}
