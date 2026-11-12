#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>

#include <kernel/arch/x86/io.h>
#include <kernel/arch/x86/irq.h>
#include <kernel/util/timer.h>

#include <kernel/scheduler/scheduler.h>
extern void scheduler_context(void);

volatile uint64_t current_ms = 0;

void timer_init(void) {
    // 1193182 Hz / 1000 Hz = 1193 divisor
    uint16_t divisor = 1193;
    
    // Command byte: Channel 0, access LoByte/HiByte, Mode 3, Binary
    outb(0x43, 0x36);
    
    // Set divisor
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

uint64_t timer_get_ms(void) {
    // A 64-bit load is two 32-bit loads on i386; keep the IRQ from
    // incrementing current_ms between them (torn read).
    uint32_t flags = irq_save();
    uint64_t value = current_ms;
    irq_restore(flags);
    return value;
}

void isr_callback_timer_handler(void) {
    outb(0x20, 0x20); // End of Interrupt
    current_ms++;
}

