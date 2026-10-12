#include <kernel/arch/x86/io.h>

void init_sse(void) {
    uint32_t cr0;
    uint32_t cr4;
    
    // Read current CR0
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    
    // Clear the EM (Emulation) bit (bit 2) -> We have a real FPU, don't emulate it
    cr0 &= ~(1 << 2);
    // Set the MP (Monitor Coprocessor) bit (bit 1) -> Controls interaction with TS bit
    cr0 |= (1 << 1);
    
    // Write back to CR0
    asm volatile("mov %0, %%cr0" :: "r"(cr0));
    
    // Read current CR4
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    
    // Set OSFXSR (bit 9) -> FXSAVE/FXRSTOR support for SSE state
    cr4 |= (1 << 9);
    // Set OSXMMEXCPT (bit 10) -> Use unmasked SIMD floating-point exceptions (#XM)
    cr4 |= (1 << 10);
    
    // Write back to CR4
    asm volatile("mov %0, %%cr4" :: "r"(cr4));
}
