#ifndef _KERNEL_ARCH_VMM_H_
#define _KERNEL_ARCH_VMM_H_

// Page allocator and kernel mappings (vmm_alloc_pages, vmm_free_pages,
// vmm_unmap_page, pmm_free_frame, PAGE_SIZE, VM_USER_*) for portable code.
//
// The calls are the same on every architecture; the page table format
// behind them is not. Only x86 has one so far.

#if defined(KERNEL_PLATFORM_X86)
  #include <kernel/arch/x86/virtual/vmm.h>
#elif defined(KERNEL_PLATFORM_AARCH64)
  #error "aarch64 has no vmm yet (kernel/arch/aarch64/virtual/vmm.h)"
#else
  #error "No KERNEL_PLATFORM_* defined: the build must name the target CPU"
#endif

#endif
