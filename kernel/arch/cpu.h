#ifndef _KERNEL_ARCH_CPU_H_
#define _KERNEL_ARCH_CPU_H_

//
// CPU primitives the portable kernel may use
//
// Portable code (scheduler, panic, gfx, ...) includes this header instead of
// writing cli / hlt / sfence inline. Each architecture implements the same
// small set of static inline functions in kernel/arch/<arch>/cpu.h:
//
//   arch_irq_flags_t           saved interrupt state (EFLAGS / DAIF)
//   arch_irq_disable()         cli            / msr daifset, #2
//   arch_irq_enable()          sti            / msr daifclr, #2
//   arch_irq_save()            pushf; cli     / mrs daif; msr daifset
//   arch_irq_restore(flags)    popf           / msr daif
//   arch_wait_for_interrupt()  hlt            / wfi
//   arch_halt_forever()        cli; hlt loop  / mask + wfi loop
//   arch_store_fence()         sfence         / dsb st
//
// The build defines exactly one KERNEL_PLATFORM_* (see CMakeLists.txt).
//

#if defined(KERNEL_PLATFORM_X86)
  #include <kernel/arch/x86/cpu.h>
#elif defined(KERNEL_PLATFORM_AARCH64)
  #include <kernel/arch/aarch64/cpu.h>
#else
  #error "No KERNEL_PLATFORM_* defined: the build must name the target CPU"
#endif

#endif
