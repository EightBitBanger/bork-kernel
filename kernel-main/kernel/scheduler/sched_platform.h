#ifndef _KERNEL_SCHED_PLATFORM_H_
#define _KERNEL_SCHED_PLATFORM_H_

#include <stdint.h>
#include <kernel/scheduler/scheduler.h>

// What the scheduler core (scheduler.c) needs from the CPU
//
// scheduler.c is the policy: run queue, priorities, aging, sleep / wake,
// events and picking the next thread. Everything that depends on the CPU's
// registers, privilege levels or MMU goes through these calls, implemented
// once per architecture:
//
//   x86       kernel/arch/x86/scheduler/sched_x86.c
//   aarch64   kernel/arch/aarch64/scheduler/sched_aarch64.c
//
// The interrupt entry stubs that save a thread's registers, call
// thread_handler_c() and switch to the stack it returns belong to the
// architecture too (boot/x86/isr/scheduler_*.asm, arch/aarch64/vectors.S).

// Bytes of FPU / SIMD state kept at the top of every thread stack (a
// multiple of 16, so the stack below it stays 16-byte aligned)
#if defined(KERNEL_PLATFORM_X86)
  #define SCHED_PLAT_FPU_STATE_SIZE   512     // FXSAVE image
#elif defined(KERNEL_PLATFORM_AARCH64)
  #define SCHED_PLAT_FPU_STATE_SIZE   528     // q0-q31, FPCR, FPSR
#else
  #error "No KERNEL_PLATFORM_* defined: the build must name the target CPU"
#endif

// Once, from scheduler_init(), before any thread is built
void sched_plat_init(void);

// FPU area for the boot thread (it runs on the boot stack, which has none)
uint8_t* sched_plat_boot_fpu_state(void);

// Fill a new thread's FPU area with the clean starting state
void sched_plat_fpu_init(uint8_t* area);

// The kernel's own address space, stored in ThreadBlock.cr3 for kernel threads
uint32_t sched_plat_kernel_space(void);

// Build the frame the first switch to `thread` restores, below its
// kernel_stack_top, and set thread->esp to it.
//
// Kernel thread: runs entry_point(arg); returning from it lands in exit_fn.
// User thread: starts at `entry` in user mode with its stack at user_stack.
void sched_plat_build_kernel_frame(ThreadBlock* thread, uint32_t entry_point, void* arg,
                                   void (*exit_fn)(void));
void sched_plat_build_user_frame(ThreadBlock* thread, uint32_t entry, uint32_t user_stack);

// `next` is about to be resumed. Interrupts are off and we are still on the
// outgoing thread's kernel stack: point user-mode entries at next's kernel
// stack and switch to its address space.
void sched_plat_switch_to(ThreadBlock* next);

// Enter the scheduler now (int $0x80 on x86, svc on aarch64). Works with
// interrupts off; returns once this thread is picked again.
void sched_plat_yield(void);

#endif
