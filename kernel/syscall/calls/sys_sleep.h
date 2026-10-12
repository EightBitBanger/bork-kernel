#ifndef _KERNEL_SYS_SLEEP_H_
#define _KERNEL_SYS_SLEEP_H_

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/scheduler/scheduler.h>

// sleep(ms): block for at least ms milliseconds (0 = just yield). Returns 0.
static int32_t sys_sleep(const SyscallArgs* args) {
    thread_sleep(args->arg1);
    return SYS_OK;
}

#endif
