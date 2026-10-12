#ifndef KERNEL_PHYSICAL_MEMORY_MGR_H
#define KERNEL_PHYSICAL_MEMORY_MGR_H

#include <stddef.h>
#include <stdint.h>
#include <kernel/boot/x86/multiboot_info.h>

#define PAGE_SIZE 4096U

// Returned by pmm_alloc_frame() when physical memory is exhausted
#define PMM_NO_FRAME 0xFFFFFFFFU

void pmm_init(struct MultibootInfo* mbi, uint32_t physical_begin);

uint32_t pmm_alloc_frame(void);

void pmm_free_frame(uint32_t phys_addr);

#endif

