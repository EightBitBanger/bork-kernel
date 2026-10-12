#ifndef GLOBAL_DESCRIPTOR_TABLE_H
#define GLOBAL_DESCRIPTOR_TABLE_H

#include <stdint.h>

// Segment selectors: (index * 8) | RPL
#define GDT_KERNEL_CODE_SEL   0x08    // Index 1, RPL 0
#define GDT_KERNEL_DATA_SEL   0x10    // Index 2, RPL 0
#define GDT_USER_CODE_SEL     0x1B    // Index 3, RPL 3
#define GDT_USER_DATA_SEL     0x23    // Index 4, RPL 3
#define GDT_TSS_SEL           0x28    // Index 5

#define GDT_ENTRY_COUNT       6

struct gdt_entry_struct {
    uint16_t limit_low;           // The lower 16 bits of the limit
    uint16_t base_low;            // The lower 16 bits of the base
    uint8_t  base_middle;         // The next 8 bits of the base
    uint8_t  access;              // Access flags, determine what ring this segment can be used in
    uint8_t  granularity;         // Low 4 bits = Limit high 4 bits; High 4 bits = Flags
    uint8_t  base_high;           // The last 8 bits of the base
} __attribute__((packed));

struct gdt_ptr_struct {
    uint16_t limit;               // Size of the GDT in bytes, minus 1
    uint32_t base;                // The address of the first gdt_entry_t struct
} __attribute__((packed));

// 32-bit Task State Segment.
//
// No hardware task switching is used. The TSS exists for one thing: when an
// interrupt, exception or int $0x80 arrives while the CPU is in ring 3, it
// loads SS:ESP from ss0:esp0 and pushes the user SS, ESP, EFLAGS, CS and EIP
// onto that kernel stack. Interrupts taken in ring 0 never look at it.
struct tss_entry_struct {
    uint32_t prev_tss;
    uint32_t esp0;                // Kernel stack top for ring 3 -> ring 0 entry
    uint32_t ss0;                 // Kernel stack segment (GDT_KERNEL_DATA_SEL)
    uint32_t esp1, ss1;
    uint32_t esp2, ss2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;          // Offset of the I/O permission bitmap
} __attribute__((packed));

typedef struct gdt_ptr_struct   gdt_ptr_t;
typedef struct gdt_entry_struct gdt_entry_t;
typedef struct tss_entry_struct tss_entry_t;

// Loads the GDT (kernel + user segments and the TSS) and the task register.
// Call exactly once: ltr marks the TSS descriptor busy, and loading a busy
// TSS again raises #GP.
void gdt_init(void);
void gdt_set_gate(int32_t num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran);

// The kernel stack the CPU switches to on entry from ring 3. It must be the
// top of the CURRENT thread's kernel stack, so the scheduler sets it on every
// switch to a thread that can run in user mode. It starts out as 0: nothing
// may enter ring 3 before the first call.
void     tss_set_kernel_stack(uint32_t esp0);
uint32_t tss_get_kernel_stack(void);

#endif
