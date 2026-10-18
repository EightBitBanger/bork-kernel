#ifndef _KERNEL_SYS_TIME_H_
#define _KERNEL_SYS_TIME_H_

#include <stdint.h>

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/syscall/calls/sys_user.h>
#include <kernel/scheduler/process.h>
#include <kernel/util/timer.h>

//
// time(uint64_t* ms) -> 0
//
// Milliseconds since boot (the 1 kHz PIT tick, timer_get_ms()), written as
// a 64-bit value: a 32-bit count in EAX would wrap after 49 days and, near
// the wrap, look like an error code. Monotonic: never goes back, not set by
// anything. Always allowed.
//
//   SYS_EFAULT   ms is not writable user memory
//

static int32_t sys_time(const SyscallArgs* args) {
    uint64_t now = timer_get_ms();

    AddressSpace* space = process_current_space();
    if (space == NULL || !sys_user_put(space, args->arg1, &now, sizeof(now)))
        return SYS_EFAULT;
    return SYS_OK;
}

#endif
