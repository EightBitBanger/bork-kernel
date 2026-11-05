#ifndef _KERNEL_AARCH64_CPU_H_
#define _KERNEL_AARCH64_CPU_H_

// AArch64 side of kernel/arch/cpu.h (include that one, not this)
//
// "Interrupts" here means IRQ (DAIF.I). FIQ, SError and debug are left as
// start.S set them.

#include <stdint.h>

typedef uint64_t arch_irq_flags_t;     // DAIF

static inline void arch_irq_disable(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }
static inline void arch_irq_enable(void)  { __asm__ volatile("msr daifclr, #2" ::: "memory"); }

static inline arch_irq_flags_t arch_irq_save(void) {
    arch_irq_flags_t flags;
    __asm__ volatile("mrs %0, daif\n\tmsr daifset, #2" : "=r"(flags) : : "memory");
    return flags;
}

static inline void arch_irq_restore(arch_irq_flags_t flags) {
    __asm__ volatile("msr daif, %0" : : "r"(flags) : "memory");
}

static inline void arch_wait_for_interrupt(void) { __asm__ volatile("wfi"); }

static inline __attribute__((noreturn)) void arch_halt_forever(void) {
    for (;;)
        __asm__ volatile("msr daifset, #0xf\n\twfi");
}

// Framebuffer is Normal non-cacheable once the MMU is on: make the stores
// reach memory before the GPU scans them out
static inline void arch_store_fence(void) { __asm__ volatile("dsb st" ::: "memory"); }

// Generic timer (CNTFRQ is set by the Pi firmware: 19.2 MHz on a Pi 3)
static inline uint64_t arch_counter(void) {
    uint64_t v;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(v) : : "memory");
    return v;
}

static inline uint64_t arch_counter_frequency(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

#endif
