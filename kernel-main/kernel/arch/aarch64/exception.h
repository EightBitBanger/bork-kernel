#ifndef _KERNEL_AARCH64_EXCEPTION_H_
#define _KERNEL_AARCH64_EXCEPTION_H_

#include <stdint.h>

// What vectors.S saves on every exception (SAVE_FRAME), lowest address first.
// Its address is what thread_handler_c() gets and returns.
struct ExceptionFrame {
    uint64_t x[31];                     // x0 - x30
    uint64_t elr;                       // Where to return (EIP)
    uint64_t spsr;                      // Saved PSTATE (EFLAGS)
    uint64_t sp_el0;                    // User stack pointer
    uint64_t reserved[2];               // Keeps the frame a multiple of 16 bytes
};

_Static_assert(sizeof(struct ExceptionFrame) == 36 * 8, "must match FRAME_SIZE in vectors.S");

// ESR_EL1 exception classes the kernel cares about
#define ESR_EC(esr)             (((esr) >> 26) & 0x3F)
#define ESR_EC_UNKNOWN          0x00    // Undefined instruction (#UD)
#define ESR_EC_SVC64            0x15    // svc (int $0x80)
#define ESR_EC_IABT_LOWER       0x20    // Instruction abort from EL0 (#PF on fetch)
#define ESR_EC_IABT_SAME        0x21
#define ESR_EC_PC_ALIGN         0x22
#define ESR_EC_DABT_LOWER       0x24    // Data abort from EL0 (#PF)
#define ESR_EC_DABT_SAME        0x25
#define ESR_EC_SP_ALIGN         0x26
#define ESR_EC_BRK64            0x3C    // brk (int3)

// Called by vectors.S. Each returns the stack pointer to resume from.
uint64_t irq_dispatch_c(struct ExceptionFrame* frame);
uint64_t sync_dispatch_c(struct ExceptionFrame* frame, uint64_t esr, uint64_t far);
void     unhandled_exception_c(struct ExceptionFrame* frame, uint64_t esr, uint64_t far)
             __attribute__((noreturn));

// Hooks for the board / kernel. All are weak in exception.c, so the boot
// stub links without them:
//   exception_irq       ask the interrupt controller what fired (Pi 3: the
//                       BCM2836 local controller + the legacy GPU controller)
//   exception_report    print a fault (default: nothing, then halt)
void exception_irq(struct ExceptionFrame* frame);
void exception_report(const char* what, struct ExceptionFrame* frame, uint64_t esr, uint64_t far);

#endif
