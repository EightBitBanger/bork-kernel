// AArch64 exception dispatch (what boot/x86/interrupt.c and isr/ do on x86)

#include <stdint.h>

#include <kernel/arch/cpu.h>
#include <kernel/arch/aarch64/exception.h>

// The scheduler's context switch. scheduler.c provides the real one; until
// it is part of the aarch64 build, every switch resumes the same frame.
__attribute__((weak)) uint32_t thread_handler_c(uint32_t current_sp) {
    return current_sp;
}

__attribute__((weak)) void exception_irq(struct ExceptionFrame* frame) {
    (void)frame;
}

__attribute__((weak)) void exception_report(const char* what, struct ExceptionFrame* frame,
                                            uint64_t esr, uint64_t far) {
    (void)what; (void)frame; (void)esr; (void)far;
}

uint64_t irq_dispatch_c(struct ExceptionFrame* frame) {
    exception_irq(frame);
    return (uint64_t)(uintptr_t)frame;
}

uint64_t sync_dispatch_c(struct ExceptionFrame* frame, uint64_t esr, uint64_t far) {
    switch (ESR_EC(esr)) {
    case ESR_EC_SVC64:
        // ELR already points past the svc. Frames live below 4 GB on the
        // Pi 3 (1 GB of RAM, identity mapped), so the 32-bit ThreadBlock.esp
        // holds them until the scheduler's addresses are widened.
        return (uint64_t)thread_handler_c((uint32_t)(uintptr_t)frame);

    case ESR_EC_BRK64:
        // Debug breakpoint: report it and carry on after the brk
        exception_report("breakpoint", frame, esr, far);
        frame->elr += 4;
        return (uint64_t)(uintptr_t)frame;

    default:
        unhandled_exception_c(frame, esr, far);
    }
}

void unhandled_exception_c(struct ExceptionFrame* frame, uint64_t esr, uint64_t far) {
    // kernel_crashout() takes over here once the panic screen is ported
    exception_report("unhandled exception", frame, esr, far);
    arch_halt_forever();
}
