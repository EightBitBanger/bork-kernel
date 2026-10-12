#include <kernel/boot/x86/gdt.h>
#include <kernel/util/string.h>

gdt_entry_t gdt_entries[GDT_ENTRY_COUNT];
gdt_ptr_t   gdt_ptr;

static tss_entry_t tss __attribute__((aligned(16)));

_Static_assert(sizeof(tss_entry_t) == 104, "32-bit TSS must be 104 bytes");

void gdt_set_gate(int32_t num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt_entries[num].base_low    = (base & 0xFFFF);
    gdt_entries[num].base_middle = (base >> 16) & 0xFF;
    gdt_entries[num].base_high   = (base >> 24) & 0xFF;

    gdt_entries[num].limit_low   = (limit & 0xFFFF);
    gdt_entries[num].granularity = (limit >> 16) & 0x0F;

    gdt_entries[num].granularity |= gran & 0xF0;
    gdt_entries[num].access      = access;
}

extern void gdt_flush(uint32_t gdt_ptr_address);

static void tss_install(int32_t num) {
    memset(&tss, 0, sizeof(tss));

    tss.ss0  = GDT_KERNEL_DATA_SEL;
    tss.esp0 = 0;   // Set by tss_set_kernel_stack() before anything enters ring 3

    // An I/O bitmap offset at or past the segment limit means "no bitmap":
    // with IOPL 0, every in/out from ring 3 raises #GP
    tss.iomap_base = (uint16_t)sizeof(tss_entry_t);

    // Access 0x89: present, DPL 0, type 9 (32-bit TSS, available)
    // Granularity 0x00: byte-granular limit (a system segment, no D/B flag)
    gdt_set_gate(num, (uint32_t)&tss, sizeof(tss_entry_t) - 1, 0x89, 0x00);
}

void gdt_init(void) {
    gdt_ptr.limit = (uint16_t)(sizeof(gdt_entries) - 1);
    gdt_ptr.base  = (uint32_t)&gdt_entries;

    gdt_set_gate(0, 0, 0, 0, 0);                // Null descriptor
    gdt_set_gate(1, 0, 0xFFFFF, 0x9A, 0xCF);    // 0x08 Kernel code: ring 0, execute/read
    gdt_set_gate(2, 0, 0xFFFFF, 0x92, 0xCF);    // 0x10 Kernel data: ring 0, read/write
    gdt_set_gate(3, 0, 0xFFFFF, 0xFA, 0xCF);    // 0x1B User code:   ring 3, execute/read
    gdt_set_gate(4, 0, 0xFFFFF, 0xF2, 0xCF);    // 0x23 User data:   ring 3, read/write
    tss_install(5);                             // 0x28 TSS

    gdt_flush((uint32_t)&gdt_ptr);

    // Load the task register. This marks the TSS descriptor busy, which is
    // why gdt_init() must only ever run once.
    __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_TSS_SEL) : "memory");
}

void tss_set_kernel_stack(uint32_t esp0) {
    tss.esp0 = esp0;
}

uint32_t tss_get_kernel_stack(void) {
    return tss.esp0;
}
