#ifndef _KERNEL_SYS_NET_H_
#define _KERNEL_SYS_NET_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/syscall/calls/sys_user.h>
#include <kernel/net/net.h>
#include <kernel/arch/x86/irq.h>
#include <kernel/arch/x86/drivers/serial.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/scheduler/process.h>
#include <kernel/malloc.h>
#include <kernel/mutex.h>
#include <kernel/util/string.h>
#include <kernel/util/timer.h>
#include <kernel/util/nospec.h>

//
// Network calls: sock_listen, sock_connect, sock_accept, sock_send,
// sock_recv, sock_close, sock_wait, sock_info (ABI in syscall.h), on the
// kernel's TCP (net/tcp.c)
//
// One SysNetProcess per process that has used a socket call, on a list
// keyed by process id, created on the first call and dropped by
// syscall_process_exit(). Same shape and rules as the graphics tables:
//
//   - the list and each table's `users` count: interrupts off
//   - everything else in a table: its mutex, held for a whole call
//   - order: table mutex -> net lock. Never the other way.
//
// SYS_SOCK_WAIT never sleeps holding the mutex: it looks at its handles
// under the mutex, lets go, sleeps a slice, and looks them up again, so a
// socket closed meanwhile by another thread is never touched.
//
// Every user buffer goes through a kernel bounce buffer (SYS_SOCK_IO_MAX):
// the network code never sees a user pointer.
//

#define SYS_NET_EXIT_WAIT_MS    1000
#define SYS_NET_WAIT_SLICE_MS   10

_Static_assert(SYS_SOCK_READABLE == TCP_EV_READ && SYS_SOCK_WRITABLE == TCP_EV_WRITE &&
               SYS_SOCK_ACCEPTABLE == TCP_EV_ACCEPT && SYS_SOCK_ERROR == TCP_EV_ERROR &&
               SYS_SOCK_CONNECTED == TCP_EV_CONNECTED, "SYS_SOCK_* must match TCP_EV_*");
_Static_assert(SYS_SOCK_BACKLOG_MAX == TCP_BACKLOG_MAX, "SYS_SOCK_BACKLOG_MAX must equal TCP_BACKLOG_MAX");
_Static_assert(sizeof(SysSockAddr) == 8 && sizeof(SysSockWaitItem) == 8 && sizeof(SysSockInfo) == 32,
               "socket structures are part of the user ABI");

typedef struct {
    struct TcpSocket* tcp;          // NULL: free slot
    bool listener;
} SysNetSocket;

typedef struct SysNetProcess {
    uint32_t       process_id;
    AddressSpace*  space;           // Spots a reused process id
    uint32_t       users;           // Calls in progress (interrupts off to touch)
    mutex_t        lock;
    SysNetSocket   sockets[SYS_SOCK_PER_PROCESS];  // Handle h is sockets[h - 1]
    uint8_t*       bounce;          // SYS_SOCK_IO_MAX bytes, on first send / recv
    struct SysNetProcess* next;
} SysNetProcess;

static SysNetProcess* sys_net_list = NULL;

static int32_t sys_net_error(int error) {
    switch (error) {
        case NET_OK:            return SYS_OK;
        case NET_ERR_NOMEM:     return SYS_ENOMEM;
        case NET_ERR_DOWN:      return SYS_ENETDOWN;
        case NET_ERR_NOROUTE:   return SYS_ENETUNREACH;
        case NET_ERR_TIMEOUT:   return SYS_ETIMEDOUT;
        case NET_ERR_AGAIN:     return SYS_EAGAIN;
        case NET_ERR_INUSE:     return SYS_EADDRINUSE;
        case NET_ERR_IO:        return SYS_EIO;
        case NET_ERR_REFUSED:   return SYS_ECONNREFUSED;
        case NET_ERR_RESET:     return SYS_ECONNRESET;
        case NET_ERR_CLOSED:    return SYS_ENOTCONN;
        default:                return SYS_EINVAL;
    }
}

//
// Tables
//

static void sys_net_close_all(SysNetProcess* t) {
    for (uint32_t i = 0; i < SYS_SOCK_PER_PROCESS; i++) {
        if (t->sockets[i].tcp != NULL)
            tcp_close(t->sockets[i].tcp);
        t->sockets[i].tcp = NULL;
    }
}

// The calling process's table, created if needed, returned LOCKED.
// NULL when out of memory.
static SysNetProcess* sys_net_acquire(void) {
    AddressSpace* space = process_current_space();
    if (space == NULL)
        return NULL;
    uint32_t process_id = (uint32_t)process_current_id();

    uint32_t irq_flags = irq_save();
    SysNetProcess* t = sys_net_list;
    while (t != NULL && t->process_id != process_id)
        t = t->next;
    if (t == NULL) {
        t = (SysNetProcess*)malloc(sizeof(SysNetProcess));
        if (t != NULL) {
            memset(t, 0, sizeof(SysNetProcess));
            t->process_id = process_id;
            t->space      = space;
            mutex_init(&t->lock);
            t->next = sys_net_list;
            sys_net_list = t;
        }
    }
    if (t != NULL)
        t->users++;
    irq_restore(irq_flags);

    if (t == NULL)
        return NULL;

    mutex_lock(&t->lock);

    // Left behind by an earlier process with this id that ended without
    // syscall_process_exit(): its sockets are not ours
    if (t->space != space) {
        sys_net_close_all(t);
        t->space = space;
    }
    return t;
}

static void sys_net_release(SysNetProcess* t) {
    mutex_unlock(&t->lock);

    uint32_t irq_flags = irq_save();
    t->users--;
    irq_restore(irq_flags);
}

typedef int32_t (*SysNetFn)(SysNetProcess* t, const SyscallArgs* args);

static int32_t sys_net_call(SysNetFn fn, const SyscallArgs* args) {
    SysNetProcess* t = sys_net_acquire();
    if (t == NULL)
        return SYS_ENOMEM;
    int32_t result = fn(t, args);
    sys_net_release(t);
    return result;
}

// Called from syscall_process_exit(). Thread context, interrupts on.
static void sys_net_process_exit(uint32_t process_id) {
    uint32_t irq_flags = irq_save();
    SysNetProcess* t = NULL;
    for (SysNetProcess** link = &sys_net_list; *link != NULL; link = &(*link)->next) {
        if ((*link)->process_id == process_id) {
            t = *link;
            *link = t->next;
            break;
        }
    }
    irq_restore(irq_flags);
    if (t == NULL)
        return;

    // Wait for the calls still inside it to leave
    uint64_t start = timer_get_ms();
    while (1) {
        irq_flags = irq_save();
        uint32_t users = t->users;
        irq_restore(irq_flags);
        if (users == 0)
            break;
        if (timer_get_ms() - start >= SYS_NET_EXIT_WAIT_MS) {
            serial_puts("syscall: process ");
            serial_put_dec(t->process_id);
            serial_puts(" ended inside a socket call; its sockets are leaked\n");
            return;
        }
        thread_sleep(1);
    }

    sys_net_close_all(t);
    free(t->bounce);
    free(t);
}

static SysNetSocket* sys_net_get(SysNetProcess* t, uint32_t handle) {
    if (handle == 0 || handle > SYS_SOCK_PER_PROCESS)
        return NULL;
    SysNetSocket* s = &t->sockets[array_index_nospec(handle - 1, SYS_SOCK_PER_PROCESS)];   // Spectre v1
    return s->tcp != NULL ? s : NULL;
}

// A free handle number, 0 when the table is full
static uint32_t sys_net_free_handle(SysNetProcess* t) {
    for (uint32_t i = 0; i < SYS_SOCK_PER_PROCESS; i++)
        if (t->sockets[i].tcp == NULL)
            return i + 1;
    return 0;
}

static bool sys_net_bounce(SysNetProcess* t) {
    if (t->bounce == NULL)
        t->bounce = (uint8_t*)malloc(SYS_SOCK_IO_MAX);
    return t->bounce != NULL;
}

//
// Calls
//

// sock_listen(port, backlog) -> handle
static int32_t sys_sock_listen_fn(SysNetProcess* t, const SyscallArgs* args) {
    uint32_t port = args->arg1;
    if (port == 0 || port > 0xFFFF)
        return SYS_EINVAL;
    if (port < SYS_SOCK_PORT_PRIVILEGED && !(process_current_security() & SYS_SEC_ADMIN))
        return SYS_EPERM;

    uint32_t handle = sys_net_free_handle(t);
    if (handle == 0)
        return SYS_EMFILE;
    int error;
    struct TcpSocket* tcp = tcp_listen((uint16_t)port, args->arg2, &error);
    if (tcp == NULL)
        return sys_net_error(error);
    t->sockets[handle - 1].tcp = tcp;
    t->sockets[handle - 1].listener = true;
    return (int32_t)handle;
}

// sock_connect(SysSockAddr*) -> handle
static int32_t sys_sock_connect_fn(SysNetProcess* t, const SyscallArgs* args) {
    SysSockAddr addr;
    if (!vmm_user_check(t->space, args->arg1, sizeof(addr), false) ||
        !vmm_user_copy_from(t->space, &addr, args->arg1, sizeof(addr)))
        return SYS_EFAULT;
    if (addr.port == 0 || addr.reserved != 0)
        return SYS_EINVAL;

    uint32_t handle = sys_net_free_handle(t);
    if (handle == 0)
        return SYS_EMFILE;
    int error;
    struct TcpSocket* tcp = tcp_connect(addr.ip, addr.port, &error);
    if (tcp == NULL)
        return sys_net_error(error);
    t->sockets[handle - 1].tcp = tcp;
    t->sockets[handle - 1].listener = false;
    return (int32_t)handle;
}

// sock_accept(listener, SysSockAddr* or 0) -> handle
static int32_t sys_sock_accept_fn(SysNetProcess* t, const SyscallArgs* args) {
    SysNetSocket* l = sys_net_get(t, args->arg1);
    if (l == NULL)
        return SYS_EBADF;
    if (!l->listener)
        return SYS_EINVAL;
    // Check the address first: a connection taken is not put back
    if (args->arg2 != 0 && !vmm_user_check(t->space, args->arg2, sizeof(SysSockAddr), true))
        return SYS_EFAULT;
    uint32_t handle = sys_net_free_handle(t);
    if (handle == 0)
        return SYS_EMFILE;

    SysSockAddr addr = { 0, 0, 0 };
    struct TcpSocket* tcp = tcp_accept(l->tcp, &addr.ip, &addr.port);
    if (tcp == NULL)
        return SYS_EAGAIN;
    t->sockets[handle - 1].tcp = tcp;
    t->sockets[handle - 1].listener = false;
    if (args->arg2 != 0)
        vmm_user_copy_to(t->space, args->arg2, &addr, sizeof(addr));
    return (int32_t)handle;
}

// sock_send(handle, buf, length) -> bytes
static int32_t sys_sock_send_fn(SysNetProcess* t, const SyscallArgs* args) {
    SysNetSocket* s = sys_net_get(t, args->arg1);
    if (s == NULL)
        return SYS_EBADF;
    if (s->listener)
        return SYS_ENOTCONN;
    uint32_t length = args->arg3 < SYS_SOCK_IO_MAX ? args->arg3 : SYS_SOCK_IO_MAX;
    if (length == 0)
        return 0;
    if (!sys_net_bounce(t))
        return SYS_ENOMEM;
    if (!vmm_user_check(t->space, args->arg2, length, false) ||
        !vmm_user_copy_from(t->space, t->bounce, args->arg2, length))
        return SYS_EFAULT;
    int result = tcp_send(s->tcp, t->bounce, length);
    return result >= 0 ? result : sys_net_error(result);
}

// sock_recv(handle, buf, size) -> bytes, 0 at the end
static int32_t sys_sock_recv_fn(SysNetProcess* t, const SyscallArgs* args) {
    SysNetSocket* s = sys_net_get(t, args->arg1);
    if (s == NULL)
        return SYS_EBADF;
    if (s->listener)
        return SYS_ENOTCONN;
    uint32_t size = args->arg3 < SYS_SOCK_IO_MAX ? args->arg3 : SYS_SOCK_IO_MAX;
    if (size == 0)
        return 0;
    if (!sys_net_bounce(t))
        return SYS_ENOMEM;
    // Check before taking anything: bytes taken out of the stack and then
    // not delivered would be lost
    if (!vmm_user_check(t->space, args->arg2, size, true))
        return SYS_EFAULT;
    int result = tcp_recv(s->tcp, t->bounce, size);
    if (result < 0)
        return sys_net_error(result);
    if (result > 0 && !vmm_user_copy_to(t->space, args->arg2, t->bounce, (uint32_t)result))
        return SYS_EFAULT;
    return result;
}

// sock_close(handle) -> 0
static int32_t sys_sock_close_fn(SysNetProcess* t, const SyscallArgs* args) {
    SysNetSocket* s = sys_net_get(t, args->arg1);
    if (s == NULL)
        return SYS_EBADF;
    tcp_close(s->tcp);
    s->tcp = NULL;
    s->listener = false;
    return SYS_OK;
}

// sock_info(handle, SysSockInfo*) -> 0
static int32_t sys_sock_info_fn(SysNetProcess* t, const SyscallArgs* args) {
    SysNetSocket* s = sys_net_get(t, args->arg1);
    if (s == NULL)
        return SYS_EBADF;
    SysSockInfo info;
    memset(&info, 0, sizeof(info));
    tcp_endpoints(s->tcp, &info.local.ip, &info.local.port, &info.remote.ip, &info.remote.port);
    info.flags = tcp_poll(s->tcp) | (s->listener ? SYS_SOCK_LISTENING : 0);
    int error = tcp_error(s->tcp);
    info.error = error != 0 ? sys_net_error(error) : 0;
    if (!vmm_user_check(t->space, args->arg2, sizeof(info), true) ||
        !vmm_user_copy_to(t->space, args->arg2, &info, sizeof(info)))
        return SYS_EFAULT;
    return SYS_OK;
}

static int32_t sys_sock_listen(const SyscallArgs* args)  { return sys_net_call(sys_sock_listen_fn, args); }
static int32_t sys_sock_connect(const SyscallArgs* args) { return sys_net_call(sys_sock_connect_fn, args); }
static int32_t sys_sock_accept(const SyscallArgs* args)  { return sys_net_call(sys_sock_accept_fn, args); }
static int32_t sys_sock_send(const SyscallArgs* args)    { return sys_net_call(sys_sock_send_fn, args); }
static int32_t sys_sock_recv(const SyscallArgs* args)    { return sys_net_call(sys_sock_recv_fn, args); }
static int32_t sys_sock_close(const SyscallArgs* args)   { return sys_net_call(sys_sock_close_fn, args); }
static int32_t sys_sock_info(const SyscallArgs* args)    { return sys_net_call(sys_sock_info_fn, args); }

// sock_wait(items, count, timeout_ms) -> items ready
static int32_t sys_sock_wait(const SyscallArgs* args) {
    uint32_t count = args->arg2;
    if (count == 0 || count > SYS_SOCK_WAIT_MAX)
        return SYS_EINVAL;
    AddressSpace* space = process_current_space();
    SysSockWaitItem items[SYS_SOCK_WAIT_MAX];
    uint32_t bytes = count * (uint32_t)sizeof(SysSockWaitItem);
    if (space == NULL || !vmm_user_check(space, args->arg1, bytes, true) ||
        !vmm_user_copy_from(space, items, args->arg1, bytes))
        return SYS_EFAULT;

    uint32_t timeout = args->arg3;
    uint64_t start = timer_get_ms();
    while (1) {
        SysNetProcess* t = sys_net_acquire();
        if (t == NULL)
            return SYS_ENOMEM;
        int32_t ready = 0;
        for (uint32_t i = 0; i < count && ready >= 0; i++) {
            SysNetSocket* s = sys_net_get(t, (uint32_t)items[i].handle);
            if (s == NULL) {
                ready = SYS_EBADF;
                break;
            }
            uint32_t wanted = (items[i].events & (SYS_SOCK_READABLE | SYS_SOCK_WRITABLE | SYS_SOCK_ACCEPTABLE)) |
                              SYS_SOCK_ERROR;
            items[i].ready = (uint16_t)(tcp_poll(s->tcp) & wanted);
            if (items[i].ready != 0)
                ready++;
        }
        sys_net_release(t);
        if (ready < 0)
            return ready;

        bool done = ready > 0 || timeout == 0 || process_current_kill_pending() ||
                    (timeout != SYS_SOCK_WAIT_FOREVER && timer_get_ms() - start >= timeout);
        if (done) {
            if (!vmm_user_copy_to(space, args->arg1, items, bytes))
                return SYS_EFAULT;
            return ready;
        }
        thread_sleep(SYS_NET_WAIT_SLICE_MS);
    }
}

#endif
