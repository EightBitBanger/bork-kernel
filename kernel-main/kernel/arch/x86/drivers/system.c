#include <stdbool.h>
#include <stdint.h>

#include <kernel/arch/x86/io.h>
#include <kernel/arch/x86/drivers/acpi.h>
#include <kernel/arch/x86/drivers/serial.h>
#include <kernel/util/system.h>

//
// Restart and power off (util/system.h)
//

#define PORT_RESET_CONTROL  0xCF9   // Intel chipsets (PIIX, ICH, PCH; QEMU too)
#define RC_SYS_RST          0x02    // Reset once RST_CPU goes 0 -> 1
#define RC_RST_CPU          0x04
#define RC_FULL_RST         0x08    // Power cycle, not just the CPU

#define PORT_KBC_STATUS     0x64    // 8042 keyboard controller
#define PORT_KBC_COMMAND    0x64
#define KBC_INPUT_FULL      0x02
#define KBC_PULSE_RESET     0xFE    // Pulse output line 0: CPU reset

static void delay_us(uint32_t us) {
    while (us-- > 0)
        io_wait();
}

static void halt_forever(void) {
    for (;;)
        __asm__ volatile ("cli; hlt");
}

static void kbc_reset(void) {
    for (uint32_t i = 0; i < 100000; i++) {
        if (!(inb(PORT_KBC_STATUS) & KBC_INPUT_FULL))
            break;
        delay_us(1);
    }
    outb(PORT_KBC_COMMAND, KBC_PULSE_RESET);
    delay_us(50000);
}

// An empty IDT and an interrupt: double fault with no handler, triple
// fault, the CPU resets. Always works, just less cleanly.
static void triple_fault(void) {
    struct __attribute__((packed)) { uint16_t limit; uint32_t base; } none = { 0, 0 };
    __asm__ volatile ("lidt %0; int3" : : "m"(none));
}

void system_restart(void) {
    __asm__ volatile ("cli");
    serial_puts("[power] restart\n");

    acpi_reset();                                           // The way the firmware asks for

    outb(PORT_RESET_CONTROL, RC_SYS_RST);                   // Then the chipset's own
    delay_us(50);
    outb(PORT_RESET_CONTROL, RC_SYS_RST | RC_RST_CPU | RC_FULL_RST);
    delay_us(50000);

    kbc_reset();                                            // Older machines
    triple_fault();
    halt_forever();
}

void system_poweroff(void) {
    __asm__ volatile ("cli");
    serial_puts("[power] power off\n");

    acpi_poweroff();

    // No FADT at all: an emulator without the tables, perhaps. Their power
    // management ports (QEMU's PIIX4/ICH9, Bochs and old QEMU,
    // VirtualBox). A real machine always has a FADT and never gets here,
    // so nothing unknown is written to a real chipset.
    if (!acpi_power()->found) {
        outw(0x604, 0x2000);
        outw(0xB004, 0x2000);
        outw(0x4004, 0x3400);
        delay_us(100000);
    }
    serial_puts("[power] still on: halted\n");
}
