#ifndef _KERNEL_SYS_IPC_H_
#define _KERNEL_SYS_IPC_H_

#include <stdint.h>

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/scheduler/process.h>
#include <kernel/ipc/ipc.h>

//
// IPC and input calls (ABI in syscall.h, implementation in ipc/ipc.c)
//
// syscall_table requires SYS_SEC_IPC or SYS_SEC_INPUT where the call always
// needs it. ipc_close and ipc_info are free (a process may always let go
// of, or look at, what it holds); ipc_wait checks each handle's type.
//

static IpcCaller sys_ipc_caller(void) {
    IpcCaller c = {
        .pid      = (uint32_t)process_current_id(),
        .space    = process_current_space(),
        .security = process_current_security(),
    };
    return c;
}

#define SYS_IPC_CALL(name, expr)                        \
    static int32_t name(const SyscallArgs* args) {      \
        (void)args;                                     \
        IpcCaller c = sys_ipc_caller();                 \
        if (c.space == NULL)                            \
            return SYS_EFAULT;                          \
        return (expr);                                  \
    }

SYS_IPC_CALL(sys_ipc_close,       ipc_sys_close(&c, args->arg1))
SYS_IPC_CALL(sys_ipc_pipe,        ipc_sys_pipe(&c, args->arg1))
SYS_IPC_CALL(sys_ipc_port_create, ipc_sys_port_create(&c, args->arg1))
SYS_IPC_CALL(sys_ipc_connect,     ipc_sys_connect(&c, args->arg1))
SYS_IPC_CALL(sys_ipc_accept,      ipc_sys_accept(&c, args->arg1))
SYS_IPC_CALL(sys_ipc_send,        ipc_sys_send(&c, args->arg1, args->arg2, args->arg3, args->arg4))
SYS_IPC_CALL(sys_ipc_recv,        ipc_sys_recv(&c, args->arg1, args->arg2, args->arg3, args->arg4))
SYS_IPC_CALL(sys_ipc_wait,        ipc_sys_wait(&c, args->arg1, args->arg2, args->arg3))
SYS_IPC_CALL(sys_shm_create,      ipc_sys_shm_create(&c, args->arg1))
SYS_IPC_CALL(sys_shm_map,         ipc_sys_shm_map(&c, args->arg1))
SYS_IPC_CALL(sys_shm_unmap,       ipc_sys_shm_unmap(&c, args->arg1))
SYS_IPC_CALL(sys_ipc_info,        ipc_sys_info(&c, args->arg1, args->arg2))
SYS_IPC_CALL(sys_input_open,      ipc_sys_input_open(&c))
SYS_IPC_CALL(sys_input_read,      ipc_sys_input_read(&c, args->arg1, args->arg2, args->arg3))
SYS_IPC_CALL(sys_tty_resize,      ipc_sys_tty_resize(&c, args->arg1, args->arg2, args->arg3))

// ioctl(fd, request, arg): only the terminal behind 0-2 answers
static int32_t sys_ioctl(const SyscallArgs* args) {
    if (args->arg1 >= SYS_FD_MAX)
        return SYS_EBADF;
    if (args->arg1 >= SYS_FD_FIRST)
        return SYS_ENOTTY;
    IpcCaller c = sys_ipc_caller();
    if (c.space == NULL)
        return SYS_EFAULT;
    return ipc_stdio_ioctl(&c, args->arg2, args->arg3);
}

#undef SYS_IPC_CALL

#endif

