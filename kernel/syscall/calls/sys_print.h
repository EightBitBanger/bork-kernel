#ifndef _KERNEL_SYS_PRINT_H_
#define _KERNEL_SYS_PRINT_H_

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/scheduler/process.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/arch/x86/drivers/serial.h>
#include <kernel/console/print.h>
#include <kernel/mutex.h>

#define SYS_PRINT_CHUNK  256

// Keeps one process's text from being interleaved with another's
static mutex_t sys_print_lock;

static void sys_print_init(void) {
    mutex_init(&sys_print_lock);
}

// print(buf, length): write length bytes from the caller's buf to the text
// console, mirrored to COM1. Returns the bytes written, or SYS_EFAULT.
//
// A NUL byte ends that chunk on the console (print() takes C strings); COM1
// still gets every byte.
static int32_t sys_print(const SyscallArgs* args) {
    uint32_t user_buffer = args->arg1;
    uint32_t length      = args->arg2;

    if (length == 0)
        return 0;
    if (length > SYS_PRINT_MAX)
        length = SYS_PRINT_MAX;

    AddressSpace* space = process_current_space();
    if (space == NULL)
        return SYS_EFAULT;

    // Every page must be mapped and user-accessible. The copy below reads
    // through the VMM, so a bad pointer can never fault in the kernel.
    if (!vmm_user_check(space, user_buffer, length, false))
        return SYS_EFAULT;

    char chunk[SYS_PRINT_CHUNK + 1];
    uint32_t done = 0;

    mutex_lock(&sys_print_lock);
    while (done < length) {
        uint32_t n = length - done;
        if (n > SYS_PRINT_CHUNK)
            n = SYS_PRINT_CHUNK;

        if (!vmm_user_copy_from(space, chunk, user_buffer + done, n))
            break;

        serial_write(chunk, n);
        chunk[n] = '\0';
        print(chunk);

        done += n;
    }
    mutex_unlock(&sys_print_lock);

    return (int32_t)done;
}

#endif

