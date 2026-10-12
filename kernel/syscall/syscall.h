#ifndef _KERNEL_SYSCALL_H_
#define _KERNEL_SYSCALL_H_

#include <stdint.h>

/*
 * System call ABI (i386, int 0x81)
 *
 *   EAX                  syscall number
 *   EBX ECX EDX ESI EDI  arguments 1-5
 *   EAX                  result
 *
 * Every other register, and the FPU/SSE state, is preserved.
 *
 * Results: an error is a small negative number, SYS_E* below, always in
 * [-4095, -1] (0xFFFFF001 - 0xFFFFFFFF unsigned). Anything else is success.
 * Calls that return an address (sbrk) can return values above 0x7FFFFFFF,
 * so test for errors with an unsigned compare: result >= (uint32_t)-4095.
 *
 * Why 0x81 and not 0x80: vector 0x80 is the scheduler's internal yield
 * (thread_yield, int $0x80). Its gate is DPL 0, so ring 3 cannot reach it;
 * a user `int 0x80` raises #GP and kills the process. The syscall gate is a
 * DPL 3 trap gate, so interrupts stay enabled while a syscall runs.
 *
 * This file is meant to be shared with user programs: the numbers and error
 * codes below are the ABI. Each call is implemented in syscall/calls/.
 */

#define SYSCALL_VECTOR      0x81

//      Number              Arguments                 Result
#define SYS_EXIT        0   // (int code)             never returns
#define SYS_PRINT       1   // (buf, length)          bytes printed (console + COM1)
#define SYS_YIELD       2   // ()                     0
#define SYS_SLEEP       3   // (ms)                   0
#define SYS_GETPID      4   // ()                     process id
#define SYS_SBRK        5   // (int increment)        previous heap break (address)
#define SYS_COUNT       6

#define SYS_OK               0
#define SYS_ENOSYS          -1  // Unknown syscall number
#define SYS_EFAULT          -2  // Bad user pointer (unmapped, kernel, or not writable)
#define SYS_EINVAL          -3  // Bad argument
#define SYS_ENOMEM          -4  // Out of memory or over the process's limit

#define SYS_ERROR_MIN       -4095

#define SYS_PRINT_MAX       4096    // Longer prints are truncated (the return value says how much went out)

// Kernel side: set up the serial output and locks the syscalls use. The IDT
// gate is installed by idt_init().
void syscall_init(void);

#endif
