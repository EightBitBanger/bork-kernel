#ifndef _KERNEL_SYS_GETPID_H_
#define _KERNEL_SYS_GETPID_H_

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/scheduler/process.h>

// getpid(): the calling process's id (always > 0).
static int32_t sys_getpid(const SyscallArgs* args) {
    (void)args;
    return (int32_t)process_current_id();
}

#endif
