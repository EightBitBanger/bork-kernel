#ifndef _KERNEL_SYSCALL_TYPES_H_
#define _KERNEL_SYSCALL_TYPES_H_

#include <stdint.h>

// What every handler in syscall/calls/ receives: the caller's EBX, ECX, EDX,
// ESI, EDI, in that order. A handler reads the ones its call defines and
// returns the value the caller gets in EAX (SYS_E* on failure).
typedef struct {
    uint32_t arg1;      // EBX
    uint32_t arg2;      // ECX
    uint32_t arg3;      // EDX
    uint32_t arg4;      // ESI
    uint32_t arg5;      // EDI
} SyscallArgs;

typedef int32_t (*SyscallFn)(const SyscallArgs* args);

#endif

