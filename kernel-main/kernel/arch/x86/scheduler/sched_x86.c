// Scheduler platform layer for the x86 kernel (see sched_platform.h)

#include <stddef.h>
#include <stdint.h>

#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/boot/x86/gdt.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/scheduler/sched_platform.h>

#include <kernel/util/string.h>

#define FPU_STATE_SIZE       SCHED_PLAT_FPU_STATE_SIZE

#define EFLAGS_IF_RESERVED   0x202  // IF set, bit 1 (always 1), IOPL 0

// scheduler_context.asm / scheduler_yield_context.asm read fpu_state at this offset
_Static_assert(offsetof(ThreadBlock, fpu_state) == 16, "update TB_FPU_STATE in the scheduler stubs");

struct ThreadInterruptFrame {
    uint32_t gs, fs, es, ds;                               // Pushed by ISR
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax; // Pushed by pushal
    uint32_t eip, cs, eflags;                              // Pushed by CPU on interrupt
} __attribute__((packed));

// An interrupt taken in ring 3 also pushes the user stack (privilege change),
// and the iret back pops it. Same layout, two more words on top.
struct ThreadUserFrame {
    struct ThreadInterruptFrame k;
    uint32_t user_esp, user_ss;                            // Pushed by CPU on a ring 3 -> 0 entry
} __attribute__((packed));

_Static_assert(sizeof(struct ThreadInterruptFrame) == 60, "frame layout");
_Static_assert(sizeof(struct ThreadUserFrame) == 68, "user frame layout");

extern uint32_t page_directory[];

static uint8_t main_fpu_state[FPU_STATE_SIZE]    __attribute__((aligned(16)));
static uint8_t default_fpu_state[FPU_STATE_SIZE] __attribute__((aligned(16)));

static inline uint32_t sched_read_cr3(void) {
    uint32_t value;
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

static inline void sched_write_cr3(uint32_t value) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}

// A clean FXSAVE image: x87 default control word, all exceptions masked
// in MXCSR, empty register stack.
void sched_plat_init(void) {
    memset(default_fpu_state, 0, FPU_STATE_SIZE);
    *(uint16_t*)&default_fpu_state[0]  = 0x037F;   // FCW
    *(uint32_t*)&default_fpu_state[24] = 0x1F80;   // MXCSR
}

uint8_t* sched_plat_boot_fpu_state(void) {
    return main_fpu_state;
}

void sched_plat_fpu_init(uint8_t* area) {
    memcpy(area, default_fpu_state, FPU_STATE_SIZE);
}

uint32_t sched_plat_kernel_space(void) {
    return (uint32_t)page_directory;
}

// Initial frame for a kernel thread.
//
// Initial stack, top down (T = kernel_stack_top, 16-byte aligned):
//
//   T - 16   arg                 first cdecl argument, 16-byte aligned
//   T - 20   exit_fn             return address for the entry function
//   below    ThreadInterruptFrame (popped by the context switch, then iret)
//
// After the iret ESP = T - 20, so ESP % 16 == 12 at the entry point: exactly
// what the i386 SysV ABI promises right after a CALL. A void(void) entry
// simply ignores the argument slot (cdecl: the caller owns it).
void sched_plat_build_kernel_frame(ThreadBlock* thread, uint32_t entry_point, void* arg,
                                   void (*exit_fn)(void)) {
    uint32_t stack_top = thread->kernel_stack_top;

    // Argument slot (16-byte aligned) plus 12 bytes of padding above it
    stack_top -= 16;
    *(uint32_t*)stack_top = (uint32_t)arg;

    // Return address for entry_point
    stack_top -= sizeof(uint32_t);
    *(uint32_t*)stack_top = (uint32_t)exit_fn;

    stack_top -= sizeof(struct ThreadInterruptFrame);
    struct ThreadInterruptFrame* frame = (struct ThreadInterruptFrame*)stack_top;
    memset(frame, 0, sizeof(struct ThreadInterruptFrame));

    frame->eip    = entry_point;
    frame->cs     = GDT_KERNEL_CODE_SEL;
    frame->eflags = EFLAGS_IF_RESERVED;
    frame->ds     = GDT_KERNEL_DATA_SEL;
    frame->es     = GDT_KERNEL_DATA_SEL;
    frame->fs     = GDT_KERNEL_DATA_SEL;
    frame->gs     = GDT_KERNEL_DATA_SEL;

    thread->esp = stack_top;
}

// Initial frame for a user thread: exactly what an interrupt taken in ring 3
// would have left at the top of the (otherwise empty) kernel stack. The first
// context switch to it pops the segment registers and pushad block, and the
// iret drops to ring 3 at entry with ESP = user_stack.
//
// The kernel stack is empty while the thread runs in ring 3, so esp0 is the
// very top of it; the CPU pushes the next ring 3 entry right there.
void sched_plat_build_user_frame(ThreadBlock* thread, uint32_t entry, uint32_t user_stack) {
    uint32_t stack_top = thread->kernel_stack_top - sizeof(struct ThreadUserFrame);
    struct ThreadUserFrame* frame = (struct ThreadUserFrame*)stack_top;
    memset(frame, 0, sizeof(struct ThreadUserFrame));

    frame->k.eip    = entry;
    frame->k.cs     = GDT_USER_CODE_SEL;
    frame->k.eflags = EFLAGS_IF_RESERVED;   // IOPL 0: in/out/cli/sti fault in ring 3
    frame->k.ds     = GDT_USER_DATA_SEL;
    frame->k.es     = GDT_USER_DATA_SEL;
    frame->k.fs     = GDT_USER_DATA_SEL;
    frame->k.gs     = GDT_USER_DATA_SEL;
    frame->user_esp = user_stack;
    frame->user_ss  = GDT_USER_DATA_SEL;

    thread->esp = stack_top;
}

void sched_plat_switch_to(ThreadBlock* next) {
    // Where the CPU lands the next time this thread is interrupted in ring 3.
    // Set for every thread with its own stack (harmless for kernel threads),
    // so it is always right the moment a user thread is resumed.
    if (next->kernel_stack_top != 0)
        tss_set_kernel_stack(next->kernel_stack_top);

    // Each process has its own page directory. The kernel half is shared, so
    // switching here, still on prev's kernel stack, is safe. Skipped when it
    // would not change anything (kernel thread to kernel thread), since every
    // CR3 load flushes the TLB.
    if (next->cr3 != 0 && next->cr3 != sched_read_cr3())
        sched_write_cr3(next->cr3);
}

void sched_plat_yield(void) {
    __asm__ volatile("int $0x80" : : : "memory");
}
