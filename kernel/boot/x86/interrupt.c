#include <stdint.h>
#include <stdbool.h>
#include <kernel/arch/x86/io.h>
#include <kernel/boot/x86/interrupt.h>
#include <kernel/arch/x86/drivers/ps2.h>
#include <kernel/panic/panic_error.h>

#include <kernel/dwm/dwm.h>
#include <kernel/console/keyboard.h>
#include <kernel/scheduler/scheduler.h>
extern void scheduler_context(void);
extern void scheduler_yield_context(void);

extern void isr_dummy(void);
extern void isr_dummy_master(void);
extern void isr_dummy_slave(void);
extern const uint32_t isr_exception_table[32];
extern void isr_div_zero(void);
extern void isr_mouse(void);
extern void isr_keyboard(void);
extern void isr_timer(void);
extern void isr_page_fault(void);
extern void isr_general_fault(void);
extern void isr_nmi(void);
extern void isr_syscall(void);

void pic_remap(void);

struct idt_entry idt[256];
struct idt_ptr idtp;

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low = (base & 0xFFFF);
    idt[num].base_high = (base >> 16) & 0xFFFF;
    
    idt[num].sel     = sel;
    idt[num].always0 = 0;
    idt[num].flags   = flags;
}

void idt_init(void) {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base  = (uint32_t)&idt;
    
    // CPU exceptions: report and halt (see isr_exceptions.asm)
    for (int i = 0; i < 32; i++) 
        idt_set_gate(i, isr_exception_table[i], 0x08, 0x8E);
    
    // Unclaimed PIC IRQs: acknowledge on the right controller(s)
    for (int i = 0x20; i < 0x28; i++) 
        idt_set_gate(i, (uint32_t)isr_dummy_master, 0x08, 0x8E);
    for (int i = 0x28; i < 0x30; i++) 
        idt_set_gate(i, (uint32_t)isr_dummy_slave, 0x08, 0x8E);
    
    // Everything else: no EOI
    for (int i = 0x30; i < 256; i++) 
        idt_set_gate(i, (uint32_t)isr_dummy, 0x08, 0x8E);
    
    idt_set_gate(0x20, (uint32_t)scheduler_context,     0x08, 0x8E);
    idt_set_gate(0x80, (uint32_t)scheduler_yield_context,   0x08, 0x8E);
    
    // System calls from ring 3 (syscall/syscall.h). 0xEF = present, DPL 3,
    // 32-bit trap gate: ring 3 may use it, and interrupts stay enabled while
    // a syscall runs (isr_syscall.asm relies on both). 0x80 stays DPL 0, so
    // user code can never call the scheduler's yield directly.
    idt_set_gate(0x81, (uint32_t)isr_syscall,               0x08, 0xEF);
    
    idt_set_gate(0x0E, (uint32_t)isr_page_fault,            0x08, 0x8E);
    idt_set_gate(0x0D, (uint32_t)isr_general_fault,         0x08, 0x8E);
    
    // NMI: hard-lockup watchdog (and chipset error reports), see watchdog.c
    idt_set_gate(0x02, (uint32_t)isr_nmi,                   0x08, 0x8E);
    
    idt_set_gate(0x21, (uint32_t)isr_keyboard,              0x08, 0x8E);
    idt_set_gate(0x2C, (uint32_t)isr_mouse,                 0x08, 0x8E);
    
    pic_remap();
    
    // Unmask Master PIC (0x21)
    // Bit 0 = Timer (IRQ 0), Bit 1 = Keyboard (IRQ 1), Bit 2 = Cascade (IRQ 2)
    // 0xF8 = 1111 1000 in binary
    outb(0x21, 0xF8);
    
    // Unmask Slave PIC (0xA1)
    // Bit 4 = Mouse (IRQ 12 is IRQ 4 on the Slave PIC: 12 - 8 = 4)
    // 0xEF = 1110 1111 in binary
    outb(0xA1, 0xEF);
    
    __asm__ __volatile__("lidt (%0)" : : "r" (&idtp));
}

static inline void interrupt_end(void) {outb(0x20, 0x20);}
static inline void slave_interrupt_end(void) {outb(0xA0, 0x20); interrupt_end();}

void isr_callback_div_zero_handler(void) {
    
    interrupt_end();
}

void c_dummy_handler(void) {
    // Not a PIC IRQ: no EOI
}

void c_dummy_master_handler(void) {
    interrupt_end();
}

void c_dummy_slave_handler(void) {
    slave_interrupt_end();
}

static const char* exception_names[32] = {
    "DIVIDE ERROR", "DEBUG", "NMI", "BREAKPOINT",
    "OVERFLOW", "BOUND RANGE EXCEEDED", "INVALID OPCODE", "DEVICE NOT AVAILABLE",
    "DOUBLE FAULT", "COPROCESSOR SEGMENT OVERRUN", "INVALID TSS", "SEGMENT NOT PRESENT",
    "STACK-SEGMENT FAULT", "GENERAL PROTECTION FAULT", "PAGE FAULT", "RESERVED",
    "x87 FLOATING-POINT ERROR", "ALIGNMENT CHECK", "MACHINE CHECK", "SIMD FLOATING-POINT ERROR",
    "VIRTUALIZATION EXCEPTION", "CONTROL PROTECTION", "RESERVED", "RESERVED",
    "RESERVED", "RESERVED", "RESERVED", "RESERVED",
    "HYPERVISOR INJECTION", "VMM COMMUNICATION", "SECURITY EXCEPTION", "RESERVED"
};

void isr_callback_exception_handler(uint32_t vector, uint32_t error_code, uint32_t eip) {
    const char* name = (vector < 32) ? exception_names[vector] : "UNKNOWN EXCEPTION";
    kernel_crashout(error_code, eip, PT_CPU_EXCEPTION, name);
    while (1);
}

void isr_callback_fault_handler(uint32_t error_code, uint32_t faulting_address, uint8_t type) {
    
    kernel_crashout(error_code, faulting_address, type, "");
    
    while(1);
    interrupt_end();
}

//
// Keyboard ring buffer
//
// Single producer (keyboard IRQ) / single consumer (DWM thread). The IRQ only
// writes head, the consumer only writes tail, so no lock is needed on this
// single-CPU kernel; the compiler barriers keep the slot write ordered before
// the index publish. When full, new keys are dropped (the consumer owns tail).

#define INPUT_KEY_QUEUE_SIZE 64   // Must be a power of two

static uint16_t key_queue[INPUT_KEY_QUEUE_SIZE];
static volatile uint32_t key_queue_head = 0;
static volatile uint32_t key_queue_tail = 0;

Event input_event = EVENT_INITIALIZER;

static void input_key_push(uint16_t key) {
    uint32_t head = key_queue_head;
    uint32_t next = (head + 1) & (INPUT_KEY_QUEUE_SIZE - 1);
    if (next == key_queue_tail)
        return;   // Full: drop

    key_queue[head] = key;
    __asm__ volatile("" ::: "memory");
    key_queue_head = next;
}

bool input_key_pop(uint16_t* key) {
    uint32_t tail = key_queue_tail;
    if (tail == key_queue_head)
        return false;

    __asm__ volatile("" ::: "memory");
    *key = key_queue[tail];
    __asm__ volatile("" ::: "memory");
    key_queue_tail = (tail + 1) & (INPUT_KEY_QUEUE_SIZE - 1);
    return true;
}

void keyboard_handler_c(void) {
    if (ps2_check_keyboard()) {
        uint16_t last_key_pressed = kb_getc();

        // Don't call into the DWM from interrupt context: it is not
        // reentrant and may be mid-update. Queue the key and wake the DWM.
        if (last_key_pressed != 0)
            input_key_push(last_key_pressed);
    }

    // Also covers mouse bytes that ps2_check_keyboard() routed to the mouse
    event_signal(&input_event);
    interrupt_end();
}

void mouse_handler_c(void) {
    mouse_event_handler();
    event_signal(&input_event);

    slave_interrupt_end();
}
