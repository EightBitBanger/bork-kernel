#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/arch/x86/drivers/serial.h>

// The calls themselves. Each header defines its static sys_* handler and is
// included only here.
//
// Adding a syscall: write syscall/calls/sys_<name>.h with a
// `static int32_t sys_<name>(const SyscallArgs* args)`, give it a number in
// syscall.h (before SYS_COUNT), include it below and add a table row.
#include <kernel/syscall/calls/sys_exit.h>
#include <kernel/syscall/calls/sys_print.h>
#include <kernel/syscall/calls/sys_yield.h>
#include <kernel/syscall/calls/sys_sleep.h>
#include <kernel/syscall/calls/sys_getpid.h>
#include <kernel/syscall/calls/sys_sbrk.h>

typedef struct {
    const char* name;       // For traces and reports
    SyscallFn   handler;
} SyscallEntry;

// Indexed by syscall number, so a missing row is a compile error
static const SyscallEntry syscall_table[] = {
    [SYS_EXIT]   = { "exit",   sys_exit   },
    [SYS_PRINT]  = { "print",  sys_print  },
    [SYS_YIELD]  = { "yield",  sys_yield  },
    [SYS_SLEEP]  = { "sleep",  sys_sleep  },
    [SYS_GETPID] = { "getpid", sys_getpid },
    [SYS_SBRK]   = { "sbrk",   sys_sbrk   },
};

_Static_assert(sizeof(syscall_table) / sizeof(syscall_table[0]) == SYS_COUNT,
               "every syscall number needs a row in syscall_table");

// What isr_syscall.asm leaves on the kernel stack: segment registers, the
// pushad block, then the ring 3 -> 0 frame the CPU pushed. The handler's
// result goes back through the eax slot, which popad loads.
struct SyscallFrame {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t eip, cs, eflags, user_esp, user_ss;
} __attribute__((packed));

_Static_assert(sizeof(struct SyscallFrame) == 68, "must match isr_syscall.asm");

extern ThreadBlock* current_thread;

void syscall_init(void) {
    serial_init();
    sys_print_init();
}

// Called from isr_syscall.asm with interrupts enabled, on the calling
// thread's kernel stack.
void syscall_dispatch(struct SyscallFrame* frame) {
    // The gate is DPL 3 so ring 3 can use it; a kernel thread has no
    // business here (and no process to act on)
    if ((frame->cs & 3) != 3 || current_thread == NULL || current_thread->process_id == 0) {
        frame->eax = (uint32_t)SYS_ENOSYS;
        return;
    }

    uint32_t number = frame->eax;
    if (number >= SYS_COUNT || syscall_table[number].handler == NULL) {
        frame->eax = (uint32_t)SYS_ENOSYS;
        return;
    }

    SyscallArgs args = {
        .arg1 = frame->ebx,
        .arg2 = frame->ecx,
        .arg3 = frame->edx,
        .arg4 = frame->esi,
        .arg5 = frame->edi,
    };

    frame->eax = (uint32_t)syscall_table[number].handler(&args);
}
