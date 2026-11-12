#ifndef _OS_NET_H
#define _OS_NET_H

/*
 * TCP connections over IPv4. Needs SEC_NET (os/security.h), which no
 * program has unless it is given it; net_close() always works. Listening
 * on a port below 1024 needs SEC_ADMIN as well.
 *
 * Sockets are handles (small ints, their own numbering, separate from file
 * descriptors and IPC handles), at most NET_SOCKETS_MAX per process, closed
 * when the process exits. The plain calls never block; the *_wait() helpers
 * block until they can complete (or their timeout passes).
 *
 *     // Server                                  // Client
 *     int l = net_listen(7000, 4);               uint32_t ip;
 *     int c = net_accept_wait(l, NULL, NULL,     net_parse_ip("192.168.1.10", &ip);
 *                             NET_FOREVER);      int s = net_connect_wait(ip, 7000, 5000);
 *     char buf[512];                             net_send_all(s, "hi", 2);
 *     ssize_t n = net_recv_wait(c, buf,          n = net_recv_wait(s, buf, sizeof(buf),
 *                    sizeof(buf), NET_FOREVER);                    NET_FOREVER);
 *     net_send_all(c, buf, n);                   net_close(s);
 *     net_close(c);
 *
 * Addresses are uint32_t in host order (192.168.1.2 is 0xC0A80102).
 * net_resolve() turns a host name into one (DNS).
 *
 * Errors: -1 with errno.
 *   EAGAIN        nothing to receive / accept yet, or the send buffer is full
 *   ECONNREFUSED  nobody listens there;  ECONNRESET  the peer reset it
 *   ETIMEDOUT     the peer stopped answering (or a *_wait() timed out)
 *   ENOTCONN      closed, or a listener used as a connection
 *   EADDRINUSE    port taken;  ENETUNREACH  no route;  ENETDOWN  no network
 *   EMFILE        no free socket handle;  ENOMEM  the kernel's table is full
 *   EBADF         not an open socket
 *   EPERM         no SEC_NET (or a port below 1024 without SEC_ADMIN)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <os/syscall.h>

#define NET_SOCKETS_MAX     SYS_SOCK_PER_PROCESS
#define NET_BACKLOG_MAX     SYS_SOCK_BACKLOG_MAX
#define NET_IO_MAX          SYS_SOCK_IO_MAX       // Bytes per net_send / net_recv call
#define NET_WAIT_MAX        SYS_SOCK_WAIT_MAX

#define NET_FOREVER         (-1)                  // Timeout: wait as long as it takes

#define NET_READABLE        SYS_SOCK_READABLE     // Data, the peer's close, or an error
#define NET_WRITABLE        SYS_SOCK_WRITABLE     // Room to send, or an error
#define NET_ACCEPTABLE      SYS_SOCK_ACCEPTABLE   // Listener: a connection waits
#define NET_ERROR           SYS_SOCK_ERROR        // Always reported
#define NET_CONNECTED       SYS_SOCK_CONNECTED    // net_info only
#define NET_LISTENING       SYS_SOCK_LISTENING    // net_info only

typedef SysSockWaitItem net_wait_item_t;
typedef SysSockInfo     net_info_t;

// Listen on `port` (every local address). backlog 1..NET_BACKLOG_MAX.
int net_listen(uint16_t port, int backlog);
// A finished connection from the listener, or -1 / EAGAIN. ip, port: the
// peer (either may be NULL).
int net_accept(int listener, uint32_t* ip, uint16_t* port);
// Start connecting; the handle is usable once net_wait says NET_WRITABLE
int net_connect(uint32_t ip, uint16_t port);
// Bytes queued (1 .. NET_IO_MAX), or -1
ssize_t net_send(int handle, const void* buf, size_t length);
// Bytes read, 0 once the peer closed and everything was read, or -1
ssize_t net_recv(int handle, void* buf, size_t size);
int net_close(int handle);
// How many items are ready (their .ready set), 0 on timeout, or -1
int net_wait(net_wait_item_t* items, size_t count, int32_t timeout_ms);
int net_info(int handle, net_info_t* out);

// Blocking helpers
int     net_wait_one(int handle, unsigned events, int32_t timeout_ms);     // ready bits, 0 on timeout
int     net_connect_wait(uint32_t ip, uint16_t port, int32_t timeout_ms);  // connected handle, or -1
int     net_accept_wait(int listener, uint32_t* ip, uint16_t* port, int32_t timeout_ms);
ssize_t net_recv_wait(int handle, void* buf, size_t size, int32_t timeout_ms);
ssize_t net_send_all(int handle, const void* buf, size_t length);          // length, or -1

// The IPv4 address of a host name ("www.example.com"), asked of the DNS
// server the network gave (else the router). "a.b.c.d" and "localhost" are
// answered at once. Blocks up to timeout_ms (NET_FOREVER or 0: the kernel's
// most, NET_RESOLVE_TIMEOUT_MAX). 0, or -1 with errno: ENOENT no such host,
// ETIMEDOUT no answer, ENETDOWN not connected, EINVAL not a host name,
// ENAMETOOLONG over NET_NAME_MAX bytes, EIO the DNS server failed.
#define NET_NAME_MAX            SYS_NET_NAME_MAX
#define NET_RESOLVE_TIMEOUT_MAX SYS_NET_RESOLVE_TIMEOUT_MAX
int net_resolve(const char* name, uint32_t* ip, int32_t timeout_ms);

// "a.b.c.d" <-> address. net_format_ip needs 16 bytes.
bool net_parse_ip(const char* text, uint32_t* out);
void net_format_ip(uint32_t ip, char* out);

#endif
