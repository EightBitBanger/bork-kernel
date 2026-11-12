/*
 * Pipes, ports, shared memory (os/ipc.h) and input (os/input.h) on the
 * SYS_IPC_* / SYS_SHM_* / SYS_INPUT_* calls.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <os/ipc.h>
#include <os/input.h>
#include "internal.h"

static uint32_t ipc_timeout(int32_t timeout_ms) {
    return (timeout_ms < 0) ? SYS_IPC_WAIT_FOREVER : (uint32_t)timeout_ms;
}

int ipc_close(int handle) {
    return __sys_ret(syscall1(SYS_IPC_CLOSE, (uint32_t)handle));
}

int ipc_pipe(int out[2]) {
    if (out == NULL) {
        errno = EFAULT;
        return -1;
    }
    int32_t ends[2];
    int r = __sys_ret(syscall1(SYS_IPC_PIPE, (uint32_t)(uintptr_t)ends));
    if (r == 0) {
        out[0] = ends[0];
        out[1] = ends[1];
    }
    return r;
}

int ipc_port_create(const char* name) {
    return __sys_ret(syscall1(SYS_IPC_PORT_CREATE, (uint32_t)(uintptr_t)name));
}

int ipc_connect(const char* name) {
    return __sys_ret(syscall1(SYS_IPC_CONNECT, (uint32_t)(uintptr_t)name));
}

int ipc_accept(int port) {
    return __sys_ret(syscall1(SYS_IPC_ACCEPT, (uint32_t)port));
}

int ipc_send(int pipe, const void* buf, size_t length, int attach) {
    if (length == 0 || length > IPC_MSG_MAX) {
        errno = EINVAL;
        return -1;
    }
    return __sys_ret(syscall4(SYS_IPC_SEND, (uint32_t)pipe, (uint32_t)(uintptr_t)buf,
                              (uint32_t)length, (uint32_t)attach));
}

ssize_t ipc_recv(int pipe, void* buf, size_t size, ipc_msg_info_t* info) {
    return __sys_ret(syscall4(SYS_IPC_RECV, (uint32_t)pipe, (uint32_t)(uintptr_t)buf,
                              (uint32_t)size, (uint32_t)(uintptr_t)info));
}

int ipc_wait(ipc_wait_item_t* items, size_t count, int32_t timeout_ms) {
    if (count == 0 || count > IPC_WAIT_MAX) {
        errno = EINVAL;
        return -1;
    }
    return __sys_ret(syscall3(SYS_IPC_WAIT, (uint32_t)(uintptr_t)items, (uint32_t)count,
                              ipc_timeout(timeout_ms)));
}

int ipc_wait_one(int handle, unsigned events, int32_t timeout_ms) {
    ipc_wait_item_t item = { handle, (uint16_t)events, 0 };
    int r = ipc_wait(&item, 1, timeout_ms);
    return (r <= 0) ? r : (int)item.ready;
}

int ipc_accept_wait(int port) {
    while (1) {
        int r = ipc_accept(port);
        if (r >= 0 || errno != EAGAIN)
            return r;
        if (ipc_wait_one(port, IPC_READABLE, IPC_FOREVER) < 0)
            return -1;
    }
}

int ipc_send_wait(int pipe, const void* buf, size_t length, int attach) {
    while (1) {
        int r = ipc_send(pipe, buf, length, attach);
        if (r >= 0 || errno != EAGAIN)
            return r;
        if (ipc_wait_one(pipe, IPC_WRITABLE, IPC_FOREVER) < 0)
            return -1;
    }
}

ssize_t ipc_recv_wait(int pipe, void* buf, size_t size, ipc_msg_info_t* info) {
    while (1) {
        ssize_t r = ipc_recv(pipe, buf, size, info);
        if (r >= 0 || errno != EAGAIN)
            return r;
        if (ipc_wait_one(pipe, IPC_READABLE, IPC_FOREVER) < 0)
            return -1;
    }
}

int ipc_info(int handle, ipc_info_t* out) {
    return __sys_ret(syscall2(SYS_IPC_INFO, (uint32_t)handle, (uint32_t)(uintptr_t)out));
}

//
// Shared memory
//

int shm_create(size_t size) {
    if (size == 0 || size > SHM_MAX) {
        errno = EINVAL;
        return -1;
    }
    return __sys_ret(syscall1(SYS_SHM_CREATE, (uint32_t)size));
}

void* shm_map(int handle) {
    int32_t r = syscall1(SYS_SHM_MAP, (uint32_t)handle);
    if (SYS_IS_ERROR(r)) {
        errno = __errno_from_sys(r);
        return NULL;
    }
    return (void*)(uintptr_t)(uint32_t)r;
}

int shm_unmap(int handle) {
    return __sys_ret(syscall1(SYS_SHM_UNMAP, (uint32_t)handle));
}

size_t shm_size(int handle) {
    ipc_info_t info;
    if (ipc_info(handle, &info) < 0 || info.type != IPC_TYPE_SHM)
        return 0;
    return info.size;
}

//
// Input
//

int input_open(void) {
    return __sys_ret(syscall0(SYS_INPUT_OPEN));
}

int input_read(int handle, input_event_t* events, size_t max) {
    if (max > INPUT_READ_MAX)
        max = INPUT_READ_MAX;
    return __sys_ret(syscall3(SYS_INPUT_READ, (uint32_t)handle, (uint32_t)(uintptr_t)events, (uint32_t)max));
}

int input_wait(int handle, int32_t timeout_ms) {
    int r = ipc_wait_one(handle, IPC_READABLE, timeout_ms);
    return (r < 0) ? -1 : (r != 0);
}

int input_close(int handle) {
    return ipc_close(handle);
}

int input_warp(int handle, int32_t x, int32_t y) {
    // The kernel checks the handle with a zero-event read into this
    input_event_t scratch;
    return __sys_ret(syscall4(SYS_INPUT_WARP, (uint32_t)handle, (uint32_t)x, (uint32_t)y,
                              (uint32_t)(uintptr_t)&scratch));
}

