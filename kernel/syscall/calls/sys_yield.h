#ifndef _KERNEL_SYS_YIELD_H_
#define _KERNEL_SYS_YIELD_H_

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/scheduler/scheduler.h>

// yield(): give up the rest of this time slice. Returns 0.
static int32_t sys_yield(const SyscallArgs* args) {
    (void)args;
    thread_yield();
    return SYS_OK;
}

#endif
