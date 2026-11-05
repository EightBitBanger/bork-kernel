// boot/rpi3/start.S  --  the Pi counterpart of boot/x86/multiboot.asm
//
// The firmware (start.elf) loads kernel8.img at 0x80000 and jumps here on
// core 0, at EL2, with the device tree pointer in x0. Cores 1-3 wait in the
// firmware's spin table until released (not done yet), but are parked here
// too in case a boot stub sends them anyway. There is no Multiboot header and
// no GRUB: config.txt plays that role.
//
// In the same order as _start on x86:
//   1. Park cores 1-3
//   2. Drop from EL3 / EL2 to EL1 (the ARM equivalent of ring 0)
//   3. Set up the stack
//   4. Enable FP/SIMD (the CR0/CR4 OSFXSR dance on x86)
//   5. Zero .bss (GRUB did this on x86; here nobody does)
//   6. Install the exception vectors (lidt) and call kmain(dtb)

.section ".text.boot"
.global _start

_start:
    mov     x19, x0                     // Device tree blob (callee-saved)

    // 1. Only core 0 continues
    mrs     x1, mpidr_el1
    and     x1, x1, #3
    cbnz    x1, park

    // 2. Find out where we are. Real firmware: EL2. Some emulators: EL3.
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #3
    b.ne    1f

    // EL3 -> EL2: non-secure, AArch64 below, HVC on, SMC off
    mov     x1, #0x5b1
    msr     scr_el3, x1
    mov     x1, #0x3c9                  // EL2h, DAIF masked
    msr     spsr_el3, x1
    adr     x1, 1f
    msr     elr_el3, x1
    eret

1:  mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #2
    b.ne    at_el1

    // EL2 -> EL1
    mov     x1, #(1 << 31)              // HCR_EL2.RW: EL1 runs AArch64
    msr     hcr_el2, x1
    // Let EL1 use the generic timer and FP without trapping to EL2
    mov     x1, #3
    msr     cnthctl_el2, x1
    msr     cntvoff_el2, xzr
    mov     x1, #0x33ff
    msr     cptr_el2, x1
    msr     hstr_el2, xzr
    // SCTLR_EL1 to a known state: MMU, caches and alignment checks off
    ldr     x1, =0x30d00800
    msr     sctlr_el1, x1
    mov     x1, #0x3c5                  // EL1h, DAIF masked (like the x86 `cli`)
    msr     spsr_el2, x1
    adr     x1, at_el1
    msr     elr_el2, x1
    eret

at_el1:
    // 3. Stack (16 KB). AArch64 requires SP % 16 == 0 at all times, so all
    //    the `and esp, 0xFFFFFFF0` fixups on x86 have no counterpart here.
    ldr     x1, =stack_top
    mov     sp, x1

    // 4. FP/SIMD on at EL1 (CPACR_EL1.FPEN = 0b11)
    mov     x1, #(3 << 20)
    msr     cpacr_el1, x1
    isb

    // 5. Clear .bss (linker.ld aligns both ends to 16)
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
2:  cmp     x1, x2
    b.hs    3f
    stp     xzr, xzr, [x1], #16
    b       2b
3:

    // 6. Exception vector table (the lidt equivalent), then kmain(dtb)
    ldr     x1, =vector_table
    msr     vbar_el1, x1
    isb

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
