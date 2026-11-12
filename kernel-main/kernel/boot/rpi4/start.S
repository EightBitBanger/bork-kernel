// boot/rpi4/start.S  --  replaces multiboot.asm
//
// The Pi firmware (start4.elf) loads kernel8.img to 0x80000, puts all four
// cores here, and passes the device tree pointer in x0. There is no Multiboot
// header and no GRUB: config.txt plays that role (see config.txt).
//
// Responsibilities, in the same order as _start on x86:
//   1. Park cores 1-3 (x86 only ever starts the BSP, so there was nothing to do)
//   2. Drop from EL2 to EL1 (the ARM equivalent of "be in ring 0 with our own tables")
//   3. Set up the stack
//   4. Enable FP/SIMD (the CR0/CR4 OSFXSR dance on x86)
//   5. Zero .bss (GRUB did this for you on x86; here nobody does)
//   6. Call kmain(dtb)

.section ".text.boot"
.global _start

_start:
    mov     x19, x0                     // Device tree blob pointer (callee-saved)

    // 1. Only core 0 continues
    mrs     x1, mpidr_el1
    and     x1, x1, #3
    cbnz    x1, park

    // 2. Firmware enters at EL2. Configure EL1 and eret into it.
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #2
    b.ne    at_el1

    mov     x1, #(1 << 31)              // HCR_EL2.RW: EL1 runs AArch64
    msr     hcr_el2, x1
    mov     x1, #0x3c5                  // EL1h, DAIF masked (like the x86 `cli`)
    msr     spsr_el2, x1
    adr     x1, at_el1
    msr     elr_el2, x1
    // Let EL1 use the generic timer and FP without trapping to EL2
    mov     x1, #3
    msr     cnthctl_el2, x1
    msr     cntvoff_el2, xzr
    mov     x1, #0x33ff
    msr     cptr_el2, x1
    eret

at_el1:
    // 3. Stack (16 KB, 16-byte aligned -- AArch64 requires SP % 16 == 0 always,
    //    so all the `and esp, 0xFFFFFFF0` fixups on x86 disappear)
    ldr     x1, =stack_top
    mov     sp, x1

    // 4. FP/SIMD on at EL1 (CPACR_EL1.FPEN = 0b11)
    mov     x1, #(3 << 20)
    msr     cpacr_el1, x1
    isb

    // 5. Clear .bss
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
1:  cmp     x1, x2
    b.hs    2f
    str     xzr, [x1], #8
    b       1b
2:

    // Install the exception vector table (the lidt equivalent)
    ldr     x1, =vector_table
    msr     vbar_el1, x1
    isb

    // 6. kmain(dtb)
    mov     x0, x19
    bl      kmain

park:
    wfe                                 // `hlt` equivalent
    b       park

.section ".bss"
.balign 16
stack_bottom:
    .space  16384
stack_top:
