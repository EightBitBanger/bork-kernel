
#ifndef _KERNEL_SYS_SECURITY_H_
#define _KERNEL_SYS_SECURITY_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/scheduler/process.h>

//
// Security clearance calls: sec_get, sec_set (ABI in syscall.h)
//
// syscall_dispatch() checks each call's fixed requirement (the `requires`
// column of syscall_table) before the handler runs. Requirements that depend
// on the arguments (open's flags, write's descriptor, present's destination)
// are checked by the handlers themselves with sys_security_allows().
//

// True when the calling process holds every bit in `bits`
static inline bool sys_security_allows(uint32_t bits) {
    return (process_current_security() & bits) == bits;
}

// pid argument -> process id; 0 means the caller
static inline ProcessId sys_security_target(uint32_t pid) {
    return (pid == 0) ? process_current_id() : (ProcessId)pid;
}

//
// sec_get(pid or 0) -> mask
//
// Its own mask, any process. Another process's mask: SYS_SEC_ADMIN only
// (checked before the lookup, so a non-admin cannot probe which ids exist).
//

static int32_t sys_sec_get(const SyscallArgs* args) {
    ProcessId self   = process_current_id();
    ProcessId target = sys_security_target(args->arg1);

    if (target != self && !sys_security_allows(SYS_SEC_ADMIN))
        return SYS_EPERM;

    ProcessSecurity security;
    if (target <= 0 || !ProcessGetSecurity(target, &security))
        return SYS_ESRCH;
    return (int32_t)security;       // SYS_SEC_VALID keeps it far below the error range
}

//
// sec_set(pid or 0, mask) -> previous mask
//
// syscall_dispatch() has already required SYS_SEC_ADMIN of the caller.
//

static int32_t sys_sec_set(const SyscallArgs* args) {
    ProcessId target = sys_security_target(args->arg1);
    uint32_t  mask   = args->arg2;

    if ((mask & ~SYS_SEC_VALID) != 0)
        return SYS_EINVAL;
    if (target <= 0)
        return SYS_ESRCH;

    ProcessSecurity previous = 0;
    switch (ProcessSetSecurity(target, mask, &previous)) {
        case PROCESS_OK:              return (int32_t)previous;
        case PROCESS_ERR_INVALID_ARG: return SYS_EINVAL;
        default:                      return SYS_ESRCH;
    }
}

_Static_assert((SYS_SEC_VALID & 0x80000000u) == 0, "a mask must never look like an error result");

#endif
