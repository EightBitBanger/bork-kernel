// boot/rpi3/boot.c  --  kmain for the Raspberry Pi 3 Model B+
//
// A boot stub so far: it brings up the serial console, asks the firmware
// what board and memory it has, proves the exception vectors work, and then
// echoes serial input. boot/x86/boot.c is the full kernel bring-up this will
// grow into; the steps still missing, in the order that file does them:
//
//   1. mmu_init()            identity map RAM + peripherals, caches on (with
//                            the MMU off every access is Device memory:
//                            uncached, and unaligned accesses fault)
//   2. interrupts            BCM2836 local controller + legacy GPU controller
//                            (pic_remap / idt_init), core timer at 1 kHz
//                            (timer_init)
//   3. pmm / vmm / heap      kernel/arch/aarch64/virtual/, same calls as x86
//   4. scheduler_init()      scheduler.c is portable; the CPU half is
//                            arch/aarch64/scheduler/sched_aarch64.c
//   5. framebuffer           over the mailbox (replaces the Multiboot video
//                            request), then display_init / draw_set_*
//   6. storage + input       EMMC (SD card) for block, DWC2 USB for HID
//
// Everything from the registry onward in boot/x86/boot.c is portable.

#include <stdint.h>
#include <stdbool.h>

#include <kernel/arch/cpu.h>
#include <kernel/arch/aarch64/exception.h>
#include <kernel/boot/rpi3/board.h>
#include <kernel/boot/rpi3/mailbox.h>
#include <kernel/boot/rpi3/uart.h>

extern char _kernel_program_end[];

void kmain(uint64_t dtb);

static uint64_t current_el(void) {
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return (el >> 2) & 3;
}

// New-style revision codes (bit 23 set): bits 4-11 are the board type
static const char* board_type_name(uint32_t revision) {
    if (!(revision & (1u << 23)))
        return "unknown (old-style revision code)";
    switch ((revision >> 4) & 0xFF) {
    case 0x08: return "Pi 3 Model B";
    case 0x0D: return "Pi 3 Model B+";
    case 0x0E: return "Pi 3 Model A+";
    case 0x12: return "Pi Zero 2 W";
    case 0x11: return "Pi 4 Model B (wrong kernel: peripherals moved)";
    default:   return "unknown";
    }
}

void exception_report(const char* what, struct ExceptionFrame* frame, uint64_t esr, uint64_t far) {
    uart_printf("exception: %s  EC=0x%x ESR=0x%llx ELR=0x%llx FAR=0x%llx\n", what,
                (unsigned)ESR_EC(esr), (unsigned long long)esr,
                (unsigned long long)frame->elr, (unsigned long long)far);
}

void kmain(uint64_t dtb) {
    uart_init();
    uart_puts("\nbork kernel: aarch64 boot stub for the " BOARD_NAME "\n");
    uart_printf("  EL%u, device tree at 0x%llx, kernel 0x%x - 0x%llx\n",
                (unsigned)current_el(), (unsigned long long)dtb, BOARD_KERNEL_BASE,
                (unsigned long long)(uintptr_t)_kernel_program_end);

    uint64_t hz = arch_counter_frequency();
    uart_printf("  generic timer %llu Hz\n", (unsigned long long)hz);

    uint32_t revision, firmware, mem_base, mem_size;
    if (mailbox_get_board_revision(&revision))
        uart_printf("  board revision 0x%06x: %s\n", revision, board_type_name(revision));
    else
        uart_puts("  board revision: mailbox did not answer\n");
    if (mailbox_get_firmware_revision(&firmware))
        uart_printf("  firmware revision 0x%08x\n", firmware);
    if (mailbox_get_arm_memory(&mem_base, &mem_size))
        uart_printf("  ARM memory 0x%08x + %u MB (the rest belongs to the GPU)\n",
                    mem_base, mem_size >> 20);

    // Vector table check: a brk must come back through sync_dispatch_c,
    // and an svc (the scheduler's yield) must return to the next instruction
    __asm__ volatile("brk #0x42");
    __asm__ volatile("svc #0" ::: "memory");
    uart_puts("  exception vectors ok (brk and svc returned)\n");

    // One second by the generic timer, to show the clock runs
    uint64_t start = arch_counter();
    while (arch_counter() - start < hz) {}
    uart_puts("  timer ok (waited 1 s)\n");

    uart_puts("Boot stub done. Serial echo, type away:\n");
    for (;;) {
        char c = uart_getc();
        uart_putc(c == '\r' ? '\n' : c);
    }
}
