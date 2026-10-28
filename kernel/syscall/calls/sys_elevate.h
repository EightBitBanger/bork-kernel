#ifndef _KERNEL_SYS_ELEVATE_H_
#define _KERNEL_SYS_ELEVATE_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/syscall/calls/sys_user.h>
#include <kernel/scheduler/process.h>
#include <kernel/security/elevate.h>

//
// elevate(reason or 0) -> 0 once SYS_SEC_ADMIN is held
// elevate_agent(op, id, arg) -> by op
//
// ABI in syscall.h ("Elevation"), the logic in security/elevate.c.
// syscall_dispatch() has already required SYS_SEC_ELEVATE of a requester,
// and ADMIN + INPUT + GFX_SCREEN of an agent.
//

static int32_t sys_elevate(const SyscallArgs* args) {
    char reason[SYS_ELEVATE_REASON_MAX];
    reason[0] = '\0';

    if (args->arg1 != 0) {
        AddressSpace* space = process_current_space();
        if (space == NULL)
            return SYS_EFAULT;
        // Too long is fine: it is cut (sys_user_string terminates it)
        int32_t length = sys_user_string(space, args->arg1, reason, sizeof(reason));
        if (length < 0 && length != SYS_ENAMETOOLONG)
            return length;
    }
    return elevate_request(reason);
}

static int32_t sys_elevate_agent(const SyscallArgs* args) {
    // Admin that came from the user's yes is not trusted to be the desktop
    if (process_current_elevated())
        return SYS_EPERM;

    switch (args->arg1) {
        case SYS_ELEVATE_OP_REGISTER:
            return elevate_agent_register();

        case SYS_ELEVATE_OP_NEXT: {
            AddressSpace* space = process_current_space();
            if (space == NULL)
                return SYS_EFAULT;
            // The buffer first: a request handed out must reach the agent
            if (!vmm_user_check(space, args->arg3, sizeof(SysElevateRequest), true))
                return SYS_EFAULT;
            SysElevateRequest request;
            int32_t found = elevate_agent_next(&request);
            if (found <= 0)
                return found;
            if (!sys_user_put(space, args->arg3, &request, sizeof(request)))
                return SYS_EFAULT;
            return 1;
        }

        case SYS_ELEVATE_OP_STATUS:
            return elevate_agent_status(args->arg2);

        case SYS_ELEVATE_OP_ANSWER:
            if (args->arg3 > 1)
                return SYS_EINVAL;
            return elevate_agent_answer(args->arg2, args->arg3 == 1);
    }
    return SYS_EINVAL;
}

#endif
