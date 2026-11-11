#ifndef _KERNEL_SYS_TIME_H_
#define _KERNEL_SYS_TIME_H_

#include <stdint.h>

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/syscall/calls/sys_user.h>
#include <kernel/scheduler/process.h>
#include <kernel/util/timer.h>
#include <kernel/util/wallclock.h>

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

//
// clock(uint64_t* ms) -> 0
//
// The wall clock: milliseconds since 1970-01-01 00:00:00, as the machine's
// clock chip keeps it (the CMOS RTC on x86, util/wallclock.h), written as a
// 64-bit value. No time zones: whatever the chip holds, local time or UTC.
// Not monotonic: it may step by a few ms when the RTC driver resyncs. Always
// allowed.
//
//   SYS_ENOENT   no clock chip was found (the caller can fall back to SYS_TIME)
//   SYS_EFAULT   ms is not writable user memory
//

static int32_t sys_clock(const SyscallArgs* args) {
    uint64_t now;
    if (!wallclock_now_ms(&now))
        return SYS_ENOENT;

    AddressSpace* space = process_current_space();
    if (space == NULL || !sys_user_put(space, args->arg1, &now, sizeof(now)))
        return SYS_EFAULT;
    return SYS_OK;
}

//
// clock_set(const uint64_t* ms) -> 0                          (needs ADMIN)
//
// Set the date and time: milliseconds since 1970-01-01 00:00:00, in the
// same terms SYS_CLOCK reads them (the chip's own time, no time zones).
// Written to the clock chip first (the CMOS RTC keeps it across power
// off), then the running clock. Only whole seconds reach the chip.
//
//   SYS_EINVAL   before 2000 or after 2099 (the CMOS clock holds two digits)
//   SYS_ENOENT   no clock chip
//   SYS_EIO      the chip would not take it (it read back different)
//   SYS_EFAULT   ms is not readable user memory
//

#define SYS_CLOCK_SET_MIN_MS    946684800000ull     // 2000-01-01 00:00:00
#define SYS_CLOCK_SET_END_MS    4102444800000ull    // 2100-01-01 00:00:00

static int32_t sys_clock_set(const SyscallArgs* args) {
    uint64_t want;
    AddressSpace* space = process_current_space();
    if (space == NULL || !vmm_user_check(space, args->arg1, sizeof(want), false) ||
        !vmm_user_copy_from(space, &want, args->arg1, sizeof(want)))
        return SYS_EFAULT;
    if (want < SYS_CLOCK_SET_MIN_MS || want >= SYS_CLOCK_SET_END_MS)
        return SYS_EINVAL;
    if (!wallclock_now_ms(NULL))
        return SYS_ENOENT;
    if (!wallclock_change(want))
        return SYS_EIO;
    return SYS_OK;
}

#endif
