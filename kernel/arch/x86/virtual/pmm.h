#ifndef KERNEL_PHYSICAL_MEMORY_MGR_H
#define KERNEL_PHYSICAL_MEMORY_MGR_H

#include <stddef.h>
#include <stdint.h>
#include <kernel/boot/x86/multiboot_info.h>

#define PAGE_SIZE 4096U

#define PMM_NO_FRAME 0xFFFFFFFFU

void pmm_init(struct MultibootInfo* mbi, uint32_t physical_begin);

// Kernel allocations (page tables, kernel heap, stacks, DMA buffers). May
// use every free frame, the reserve included.
uint32_t pmm_alloc_frame(void);

// Frames that end up owned by a user process. Fails while no more than the
// kernel reserve is left, so user memory can never starve the kernel.
uint32_t pmm_alloc_user_frame(void);

void pmm_free_frame(uint32_t phys_addr);

// Frames free right now, and how many of them only the kernel may take
uint32_t pmm_free_count(void);
uint32_t pmm_reserve_count(void);

#endif
