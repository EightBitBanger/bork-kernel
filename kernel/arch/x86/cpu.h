#ifndef _KERNEL_X86_CPU_H_
#define _KERNEL_X86_CPU_H_

// x86 side of kernel/arch/cpu.h (include that one, not this)

#include <stdint.h>
#include <kernel/arch/x86/irq.h>

typedef uint32_t arch_irq_flags_t;     // EFLAGS

static inline void arch_irq_disable(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void arch_irq_enable(void)  { __asm__ volatile("sti" ::: "memory"); }

static inline arch_irq_flags_t arch_irq_save(void)         { return irq_save(); }
static inline void arch_irq_restore(arch_irq_flags_t flags) { irq_restore(flags); }

static inline void arch_wait_for_interrupt(void) { __asm__ volatile("hlt"); }

static inline __attribute__((noreturn)) void arch_halt_forever(void) {
    for (;;)
        __asm__ volatile("cli; hlt");
}

// VRAM is mapped write-combining: drain the WC buffers
static inline void arch_store_fence(void) { __asm__ volatile("sfence" ::: "memory"); }

#endif
