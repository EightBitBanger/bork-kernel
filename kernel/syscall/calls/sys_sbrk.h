#ifndef _KERNEL_SYS_SBRK_H_
#define _KERNEL_SYS_SBRK_H_

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/scheduler/process.h>

// sbrk(int increment): grow (or, negative, shrink) the process heap and
// return the PREVIOUS break, i.e. the start of the memory just added.
// sbrk(0) returns the current break.
//
// This is the kernel half of malloc. The heap starts right after the image
// (see process.h for the layout) and the kernel maps zeroed, read/write
// pages to cover [heap start, break). malloc()/free() themselves belong in
// the program: a user library carves this region into blocks and calls
// sbrk only when it runs out, which is how Unix-like systems do it.
//
// Errors: SYS_ENOMEM (no memory, or past PROCESS_HEAP_MAX), SYS_EINVAL
// (shrinking below the heap start). The result is an address, so compare it
// unsigned against SYS_ERROR_MIN (see syscall.h).
static int32_t sys_sbrk(const SyscallArgs* args) {
    uint32_t previous = 0;
    int result = process_sbrk((int32_t)args->arg1, &previous);

    switch (result) {
        case PROCESS_OK:              return (int32_t)previous;
        case PROCESS_ERR_NO_MEMORY:   return SYS_ENOMEM;
        case PROCESS_ERR_INVALID_ARG: return SYS_EINVAL;
        default:                      return SYS_EFAULT;
    }
}

#endif
