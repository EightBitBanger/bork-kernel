#include <kernel/arch/x86/virtual/pmm.h>
#include <kernel/panic/panic_error.h>
#include <kernel/util/string.h>
#include <kernel/arch/x86/irq.h>

#define MAX_PHYS_MEMORY    0x100000000ULL                         // 4 GB
#define PHYS_TOTAL_FRAMES  (MAX_PHYS_MEMORY / PAGE_SIZE)          // 1M frames
#define PHYS_BITMAP_WORDS  ((uint32_t)(PHYS_TOTAL_FRAMES / 32U))  // 32768 words = 128 KB

// Kernel reserve: 1/16 of the free frames at boot, kept within these bounds
#define PMM_RESERVE_DIVISOR   16U
#define PMM_RESERVE_MIN       256U                                    // 1 MB
#define PMM_RESERVE_MAX       4096U                                   // 16 MB

static uint32_t pmm_bitmap[PHYS_BITMAP_WORDS];
static uint32_t total_frames = 0;
static uint32_t free_frames = 0;
static uint32_t reserve_frames = 0;

void pmm_init(struct MultibootInfo* mbi, uint32_t physical_begin) {
    // Default to marking ALL physical memory as reserved/used (all bits 1)
    memset(pmm_bitmap, 0xFF, sizeof(pmm_bitmap));
    
    if (!mbi || !(mbi->flags & (1 << 6))) {
        kernel_crashout(0x00, 0x00000000, 0x03, "Invalid memory map");
        return;
    }
    
    struct MultibootMmapEntry* mmap = (struct MultibootMmapEntry*)mbi->mmap_addr;
    uint32_t mmap_end = mbi->mmap_addr + mbi->mmap_length;
    
    // Free memory above the kernels core space
    while ((uint32_t)mmap < mmap_end) {
        if (mmap->type == MULTIBOOT_MEMORY_AVAILABLE) {
            uint64_t region_start = mmap->addr;
            uint64_t region_end = mmap->addr + mmap->len;
            
            if (region_end > MAX_PHYS_MEMORY) {
                region_end = MAX_PHYS_MEMORY;
            }
            
            // Align region_start UP to the next page, and region_end DOWN
            // This ensures we only mark fully-formed pages as free
            // Note: the mask must be 64-bit, otherwise ~(PAGE_SIZE - 1) zero-extends
            // and clears the upper half of the address.
            uint64_t aligned_start = (region_start + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
            uint64_t aligned_end = region_end & ~(uint64_t)(PAGE_SIZE - 1);
            
            for (uint64_t addr = aligned_start; addr < aligned_end; addr += PAGE_SIZE) {
                if (addr < physical_begin) 
                    continue;
                
                uint32_t frame = (uint32_t)(addr >> 12);
                
                // Clear the allocation (counted once, even if regions overlap)
                if (pmm_bitmap[frame / 32] & (1U << (frame % 32))) {
                    pmm_bitmap[frame / 32] &= ~(1U << (frame % 32));
                    free_frames++;
                }
                
                if (frame >= total_frames) {
                    total_frames = frame + 1;
                }
            }
        }
        mmap = (struct MultibootMmapEntry*)((uint32_t)mmap + mmap->size + sizeof(mmap->size));
    }

    reserve_frames = free_frames / PMM_RESERVE_DIVISOR;
    if (reserve_frames < PMM_RESERVE_MIN) reserve_frames = PMM_RESERVE_MIN;
    if (reserve_frames > PMM_RESERVE_MAX) reserve_frames = PMM_RESERVE_MAX;
}

// Interrupts off
static uint32_t pmm_take_frame(void) {
    for (uint32_t i = 0; i < PHYS_BITMAP_WORDS; i++) {
        if (pmm_bitmap[i] == 0xFFFFFFFF) 
            continue;
        
        for (int bit = 0; bit < 32; bit++) {
            if ((pmm_bitmap[i] & (1U << bit)) == 0) {
                uint32_t frame = (i * 32) + bit;
                pmm_bitmap[i] |= (1U << bit); // Mark as used
                free_frames--;
                return frame * PAGE_SIZE;     // Returns physical address
            }
        }
    }
    
    // Out of physical frames
    return PMM_NO_FRAME;
}

uint32_t pmm_alloc_frame(void) {
    uint32_t flags = irq_save();
    uint32_t frame = pmm_take_frame();
    irq_restore(flags);
    return frame;
}

uint32_t pmm_alloc_user_frame(void) {
    uint32_t flags = irq_save();
    uint32_t frame = (free_frames > reserve_frames) ? pmm_take_frame() : PMM_NO_FRAME;
    irq_restore(flags);
    return frame;
}

void pmm_free_frame(uint32_t phys_addr) {
    uint32_t frame = phys_addr / PAGE_SIZE;
    if (frame < total_frames) {
        uint32_t flags = irq_save();
        if (pmm_bitmap[frame / 32] & (1U << (frame % 32))) {
            pmm_bitmap[frame / 32] &= ~(1U << (frame % 32));
            free_frames++;
        }
        irq_restore(flags);
    }
}

uint32_t pmm_free_count(void) {
    return free_frames;
}

uint32_t pmm_reserve_count(void) {
    return reserve_frames;
}

