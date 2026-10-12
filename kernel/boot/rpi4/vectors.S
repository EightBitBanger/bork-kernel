// boot/rpi4/vectors.S  --  replaces the whole boot/x86/isr/ directory
//
// x86 has 256 IDT gates, one stub per vector. AArch64 has one table of 16
// slots (4 kinds x 4 origins). Every IRQ enters the same `irq` slot and C asks
// the interrupt controller (GIC-400) which line fired. So isr_keyboard,
// isr_mouse, isr_timer, isr_dummy_* all collapse into one stub + a switch in C.
//
// The saved frame plays the same role as ThreadInterruptFrame: its address is
// the thread's saved SP, and thread_handler_c returns the next thread's SP.

.macro SAVE_FRAME
    sub     sp, sp, #(36 * 8)
    stp     x0,  x1,  [sp, #16 * 0]
    stp     x2,  x3,  [sp, #16 * 1]
    stp     x4,  x5,  [sp, #16 * 2]
    stp     x6,  x7,  [sp, #16 * 3]
    stp     x8,  x9,  [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    mrs     x21, elr_el1                // like EIP pushed by the CPU
    mrs     x22, spsr_el1               // like EFLAGS
    stp     x30, x21, [sp, #16 * 15]
    str     x22,      [sp, #16 * 16]
.endm

.macro RESTORE_FRAME
    ldr     x22,      [sp, #16 * 16]
    ldp     x30, x21, [sp, #16 * 15]
    msr     elr_el1, x21
    msr     spsr_el1, x22
    ldp     x0,  x1,  [sp, #16 * 0]
    ldp     x2,  x3,  [sp, #16 * 1]
    ldp     x4,  x5,  [sp, #16 * 2]
    ldp     x6,  x7,  [sp, #16 * 3]
    ldp     x8,  x9,  [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    add     sp, sp, #(36 * 8)
    eret                                // iret
.endm

.macro VENTRY label
    .balign 0x80
    b       \label
.endm

.balign 0x800
.global vector_table
vector_table:
    // Current EL, SP0 (unused)
    VENTRY  bad_exception
    VENTRY  bad_exception
    VENTRY  bad_exception
    VENTRY  bad_exception
    // Current EL, SPx  <- the kernel lives here
    VENTRY  sync_handler                // #PF, #GP, #UD, #DE, and `svc` (int $0x80)
    VENTRY  irq_handler                 // timer, keyboard, mouse, everything
    VENTRY  bad_exception               // FIQ
    VENTRY  bad_exception               // SError (async bus fault)
    // Lower EL, AArch64 (user mode, later)
    VENTRY  bad_exception
    VENTRY  bad_exception
    VENTRY  bad_exception
    VENTRY  bad_exception
    // Lower EL, AArch32
    VENTRY  bad_exception
    VENTRY  bad_exception
    VENTRY  bad_exception
    VENTRY  bad_exception

// IRQ: same shape as scheduler_context
irq_handler:
    SAVE_FRAME
    mov     x0, sp
    bl      irq_dispatch_c              // returns the SP to resume (maybe another thread)
    mov     sp, x0
    RESTORE_FRAME

// Synchronous: `svc #0` is the yield (scheduler_yield_context); anything else
// is a fault and goes to the crash screen, like isr_exceptions.asm
sync_handler:
    SAVE_FRAME
    mov     x0, sp
    mrs     x1, esr_el1                 // Exception syndrome: says *what* happened
    mrs     x2, far_el1                 // Faulting address (CR2 equivalent)
    bl      sync_dispatch_c
    mov     sp, x0
    RESTORE_FRAME

bad_exception:
    mrs     x0, esr_el1
    mrs     x1, elr_el1
    mrs     x2, far_el1
    bl      unhandled_exception_c
1:  wfi
    b       1b
