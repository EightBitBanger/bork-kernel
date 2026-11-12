#ifndef _KERNEL_X86_IRQ_H_
#define _KERNEL_X86_IRQ_H_

#include <stdint.h>

// Disable interrupts and return the previous EFLAGS so the caller can
// restore the exact prior state. Safe to nest.
//
// On this single-CPU kernel, "interrupts off" is the global lock: the
// scheduler only switches threads from the timer IRQ or int $0x80, so code
// running between irq_save()/irq_restore() cannot be preempted.
static inline uint32_t irq_save(void) {
    uint32_t eflags;
    __asm__ volatile (
        "pushfl\n\t"
        "popl %0\n\t"
        "cli"
        : "=r"(eflags)
        :
        : "memory"
    );
    return eflags;
}

static inline void irq_restore(uint32_t eflags) {
    __asm__ volatile (
        "pushl %0\n\t"
        "popfl"
        :
        : "r"(eflags)
        : "memory", "cc"
    );
}

#endif
