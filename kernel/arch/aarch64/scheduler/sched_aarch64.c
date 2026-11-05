// Scheduler platform layer for aarch64 (see sched_platform.h)
//
// Built and linked into the rpi3 stub to keep it compiling, but nothing
// calls it until scheduler.c joins that build (it needs the vmm and heap).
//
// Kernel threads run at EL1 on their own stack (SP_EL1); user threads at
// EL0 on SP_EL0. Nothing like the TSS is needed: an exception from EL0
// lands on SP_EL1, and SP_EL1 is the thread's kernel stack top because its
// user frame was popped from right there.

#include <stddef.h>
#include <stdint.h>

#include <kernel/arch/aarch64/exception.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/scheduler/sched_platform.h>

#include <kernel/util/string.h>

#define FPU_STATE_SIZE      SCHED_PLAT_FPU_STATE_SIZE

// SPSR for the first eret into a thread
#define SPSR_EL1H           0x5u        // EL1 using SP_EL1
#define SPSR_EL0T           0x0u        // EL0 using SP_EL0
#define SPSR_DAF_MASKED     0x340u      // D, A, F masked; I (IRQ) clear, so interrupts on
#define SPSR_D_MASKED       (1u << 9)

static uint8_t main_fpu_state[FPU_STATE_SIZE] __attribute__((aligned(16)));

// The kernel's TTBR0 once the MMU is on. 0 means "leave TTBR0 alone",
// which scheduler.c already treats as "no switch".
static uint32_t kernel_space = 0;

static inline uint64_t read_ttbr0(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(v));
    return v;
}

void sched_plat_init(void) {
    kernel_space = 0;
}

uint8_t* sched_plat_boot_fpu_state(void) {
    return main_fpu_state;
}

// All zero is the reset state: FPCR rounding to nearest, no traps
void sched_plat_fpu_init(uint8_t* area) {
    memset(area, 0, FPU_STATE_SIZE);
}

uint32_t sched_plat_kernel_space(void) {
    return kernel_space;
}

// The first switch restores this frame and erets to entry_point(arg) with
// x30 = exit_fn, so returning from the entry function lands in exit_fn.
// SP after the restore is kernel_stack_top, 16-byte aligned as AAPCS64 wants.
void sched_plat_build_kernel_frame(ThreadBlock* thread, uint32_t entry_point, void* arg,
                                   void (*exit_fn)(void)) {
    uintptr_t stack_top = thread->kernel_stack_top - sizeof(struct ExceptionFrame);
    struct ExceptionFrame* frame = (struct ExceptionFrame*)stack_top;
    memset(frame, 0, sizeof(*frame));

    frame->x[0]  = (uint64_t)(uintptr_t)arg;
    frame->x[30] = (uint64_t)(uintptr_t)exit_fn;
    frame->elr   = entry_point;
    frame->spsr  = SPSR_EL1H | SPSR_DAF_MASKED;

    thread->esp = (uint32_t)stack_top;
}

void sched_plat_build_user_frame(ThreadBlock* thread, uint32_t entry, uint32_t user_stack) {
    uintptr_t stack_top = thread->kernel_stack_top - sizeof(struct ExceptionFrame);
    struct ExceptionFrame* frame = (struct ExceptionFrame*)stack_top;
    memset(frame, 0, sizeof(*frame));

    frame->elr    = entry;
    frame->spsr   = SPSR_EL0T | SPSR_D_MASKED;
    frame->sp_el0 = user_stack;

    thread->esp = (uint32_t)stack_top;
}

void sched_plat_switch_to(ThreadBlock* next) {
    // Per-process address space: the user half lives in TTBR0. The kernel
    // half will live in TTBR1 and never changes. Untested until there is
    // an MMU setup to switch between.
    if (next->cr3 != 0 && next->cr3 != read_ttbr0()) {
        __asm__ volatile(
            "msr ttbr0_el1, %0\n\t"
            "isb\n\t"
            "tlbi vmalle1\n\t"
            "dsb ish\n\t"
            "isb"
            : : "r"((uint64_t)next->cr3) : "memory");
    }
}

void sched_plat_yield(void) {
    __asm__ volatile("svc #0" : : : "memory");
}
