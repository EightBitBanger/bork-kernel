#ifndef WGET_TLS_H
#define WGET_TLS_H

/*
 * https for wget: a TLS connection over one of the kernel's TCP sockets,
 * through the tls library (<home>/sys/lib/tls, os/tlslib.h), loaded the
 * first time it is needed.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct wget_tls wget_tls_t;

// Talk TLS to `host` on the connected socket. NULL with *why set (words
// for the user) when it can't: no library, no clock, out of memory.
// The handshake itself runs with the first write.
wget_tls_t* wget_tls_open(int socket, const char* host, int32_t timeout_ms, const char** why);

// 0, or -1 with *why (the certificate isn't trusted, the connection ended ...)
int wget_tls_send(wget_tls_t* t, const void* data, size_t length, const char** why);

// Bytes read (> 0); 0 when the server is done; -1 with *why. *timed_out:
// the server went quiet for the timeout.
int wget_tls_recv(wget_tls_t* t, void* out, size_t size, bool* timed_out, const char** why);

// close_notify and free; the socket is left open for the caller to close
void wget_tls_close(wget_tls_t* t);

#endif
