#ifndef _KERNEL_AARCH64_MMIO_H_
#define _KERNEL_AARCH64_MMIO_H_

// Memory-mapped register access (the ARM stand-in for x86 inb / outb)
//
// Peripherals are 32-bit registers. The barriers keep accesses to different
// peripherals in order, which the BCM283x needs (its AXI bus can return
// reads from two peripherals out of order).

#include <stdint.h>

static inline void mmio_write32(uintptr_t addr, uint32_t value) {
    __asm__ volatile("dsb st" ::: "memory");
    *(volatile uint32_t*)addr = value;
}

static inline uint32_t mmio_read32(uintptr_t addr) {
    uint32_t value = *(volatile uint32_t*)addr;
    __asm__ volatile("dsb ld" ::: "memory");
    return value;
}

// Busy-wait about `cycles` instructions (GPIO pull-up/down sequencing)
static inline void mmio_delay(uint32_t cycles) {
    while (cycles--)
        __asm__ volatile("nop");
}

#endif
