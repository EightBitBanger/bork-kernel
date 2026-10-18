#ifndef _KERNEL_SYS_EXIT_H_
#define _KERNEL_SYS_EXIT_H_

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/scheduler/process.h>

// exit(int code): end the calling process. Never returns.
//
// DestroyProcess() closes the process's open files (syscall_process_exit),
// then destroys every thread of the process and frees its address space
// (image, stack and heap pages alike).
static int32_t sys_exit(const SyscallArgs* args) {
    process_exit_current((int32_t)args->arg1);  // Does not return for a process thread
    return SYS_EINVAL;
}

#endif

