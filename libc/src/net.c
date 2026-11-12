#include <os/net.h>
#include <errno.h>
#include <string.h>
#include "internal.h"

static uint32_t net_timeout(int32_t timeout_ms) {
    return (timeout_ms < 0) ? SYS_SOCK_WAIT_FOREVER : (uint32_t)timeout_ms;
}

int net_listen(uint16_t port, int backlog) {
    return __sys_ret(syscall2(SYS_SOCK_LISTEN, port, backlog < 0 ? 0 : (uint32_t)backlog));
}

int net_accept(int listener, uint32_t* ip, uint16_t* port) {
    SysSockAddr addr = { 0, 0, 0 };
    int r = __sys_ret(syscall2(SYS_SOCK_ACCEPT, (uint32_t)listener, (uint32_t)(uintptr_t)&addr));
    if (r >= 0) {
        if (ip)   *ip = addr.ip;
        if (port) *port = addr.port;
    }
    return r;
}

int net_connect(uint32_t ip, uint16_t port) {
    SysSockAddr addr = { ip, port, 0 };
    return __sys_ret(syscall1(SYS_SOCK_CONNECT, (uint32_t)(uintptr_t)&addr));
}

ssize_t net_send(int handle, const void* buf, size_t length) {
    return __sys_ret(syscall3(SYS_SOCK_SEND, (uint32_t)handle, (uint32_t)(uintptr_t)buf,
                              length > NET_IO_MAX ? NET_IO_MAX : (uint32_t)length));
}

ssize_t net_recv(int handle, void* buf, size_t size) {
    return __sys_ret(syscall3(SYS_SOCK_RECV, (uint32_t)handle, (uint32_t)(uintptr_t)buf,
                              size > NET_IO_MAX ? NET_IO_MAX : (uint32_t)size));
}

int net_close(int handle) {
    return __sys_ret(syscall1(SYS_SOCK_CLOSE, (uint32_t)handle));
}

int net_wait(net_wait_item_t* items, size_t count, int32_t timeout_ms) {
    if (count == 0 || count > NET_WAIT_MAX) {
        errno = EINVAL;
        return -1;
    }
    return __sys_ret(syscall3(SYS_SOCK_WAIT, (uint32_t)(uintptr_t)items, (uint32_t)count,
                              net_timeout(timeout_ms)));
}

int net_info(int handle, net_info_t* out) {
    return __sys_ret(syscall2(SYS_SOCK_INFO, (uint32_t)handle, (uint32_t)(uintptr_t)out));
}

//
// Blocking helpers
//

int net_wait_one(int handle, unsigned events, int32_t timeout_ms) {
    net_wait_item_t item = { handle, (uint16_t)events, 0 };
    int r = net_wait(&item, 1, timeout_ms);
    return (r <= 0) ? r : (int)item.ready;
}

// The connection's own error, as errno
static int net_fail(int handle) {
    net_info_t info;
    errno = (net_info(handle, &info) == 0 && info.error != 0) ? __errno_from_sys(info.error) : EIO;
    return -1;
}

int net_connect_wait(uint32_t ip, uint16_t port, int32_t timeout_ms) {
    int s = net_connect(ip, port);
    if (s < 0)
        return -1;
    int r = net_wait_one(s, NET_WRITABLE, timeout_ms);
    if (r > 0 && !(r & NET_ERROR))
        return s;
    int saved = 0;
    if (r == 0)
        saved = ETIMEDOUT;
    else if (r > 0) {
        net_fail(s);
        saved = errno;
    } else
        saved = errno;
    net_close(s);
    errno = saved;
    return -1;
}

int net_accept_wait(int listener, uint32_t* ip, uint16_t* port, int32_t timeout_ms) {
    while (1) {
        int r = net_accept(listener, ip, port);
        if (r >= 0 || errno != EAGAIN)
            return r;
        int w = net_wait_one(listener, NET_ACCEPTABLE, timeout_ms);
        if (w < 0)
            return -1;
        if (w == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
    }
}

ssize_t net_recv_wait(int handle, void* buf, size_t size, int32_t timeout_ms) {
    while (1) {
        ssize_t r = net_recv(handle, buf, size);
        if (r >= 0 || errno != EAGAIN)
            return r;
        int w = net_wait_one(handle, NET_READABLE, timeout_ms);
        if (w < 0)
            return -1;
        if (w == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
    }
}

ssize_t net_send_all(int handle, const void* buf, size_t length) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t done = 0;
    while (done < length) {
        ssize_t r = net_send(handle, p + done, length - done);
        if (r > 0) {
            done += (size_t)r;
            continue;
        }
        if (r < 0 && errno != EAGAIN)
            return -1;
        if (net_wait_one(handle, NET_WRITABLE, NET_FOREVER) < 0)
            return -1;
    }
    return (ssize_t)length;
}

//
// Addresses
//

int net_resolve(const char* name, uint32_t* ip, int32_t timeout_ms) {
    if (name == NULL || ip == NULL) {
        errno = EINVAL;
        return -1;
    }
    uint32_t timeout = timeout_ms <= 0 ? 0 : (uint32_t)timeout_ms;    // 0: the kernel's most
    return __sys_ret(syscall3(SYS_NET_RESOLVE, (uint32_t)(uintptr_t)name, (uint32_t)(uintptr_t)ip, timeout));
}

bool net_parse_ip(const char* text, uint32_t* out) {
    if (text == NULL || out == NULL)
        return false;
    uint32_t ip = 0;
    for (int part = 0; part < 4; part++) {
        if (*text < '0' || *text > '9')
            return false;
        uint32_t value = 0;
        int digits = 0;
        while (*text >= '0' && *text <= '9') {
            value = value * 10 + (uint32_t)(*text++ - '0');
            if (++digits > 3 || value > 255)
                return false;
        }
        ip = (ip << 8) | value;
        if (part < 3 && *text++ != '.')
            return false;
    }
    if (*text != '\0')
        return false;
    *out = ip;
    return true;
}

void net_format_ip(uint32_t ip, char* out) {
    char* p = out;
    for (int part = 3; part >= 0; part--) {
        uint32_t v = (ip >> (part * 8)) & 0xFF;
        if (v >= 100) *p++ = (char)('0' + v / 100);
        if (v >= 10)  *p++ = (char)('0' + (v / 10) % 10);
        *p++ = (char)('0' + v % 10);
        if (part > 0) *p++ = '.';
    }
    *p = '\0';
}
