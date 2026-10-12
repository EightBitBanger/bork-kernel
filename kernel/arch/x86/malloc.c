#include <kernel/malloc.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/arch/x86/slab.h>
#include <kernel/arch/x86/irq.h>

#include <kernel/util/string.h>
#include <stdbool.h>

#define SLAB_MAX_SIZE 512

static struct SlabCache malloc_caches[] = {
    { .object_size = 16,             .page_list = NULL },
    { .object_size = 32,             .page_list = NULL },
    { .object_size = 64,             .page_list = NULL },
    { .object_size = 128,            .page_list = NULL },
    { .object_size = 256,            .page_list = NULL },
    { .object_size = SLAB_MAX_SIZE,  .page_list = NULL }
};

struct LargeAllocHeader {
    size_t num_pages;
    uint32_t magic;
};

static void* malloc_unlocked(size_t size) {
    if (size == 0) return NULL;
    
    // Find a suitable slab
    if (size <= SLAB_MAX_SIZE) {
        for (int i = 0; i < sizeof(malloc_caches) / sizeof( struct SlabCache); i++) {
            if (size <= malloc_caches[i].object_size) {
                return slab_alloc(&malloc_caches[i]);
            }
        }
    }
    
    // Size is larger than the largest slab
    
    // We need 'size' bytes, plus 1 entire page at the front to hold our metadata
    size_t total_size = size + PAGE_SIZE;
    size_t num_pages = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;
    
    size_t* raw_mem = (size_t*)vmm_alloc_pages(num_pages);
    if (!raw_mem) return NULL;
    
    memset(raw_mem, 0, num_pages * PAGE_SIZE);
    
    // Store the total number of pages at the very beginning of the first page
    raw_mem[0] = num_pages;
    
    // Return the start of the SECOND page. This pointer is page aligned
    return (void*)((uint8_t*)raw_mem + PAGE_SIZE);
}

static void free_unlocked(void* ptr) {
    if (!ptr) return;
    
    // Check if the pointer is page aligned
    // A standard slab slot is never page aligned due to the pre appended SlabPage header
    if (((uint32_t)ptr & (PAGE_SIZE - 1)) == 0) {
        // It's a large allocation, the metadata page is exactly 1 page backward
        void* raw_page_start = (void*)((uint8_t*)ptr - PAGE_SIZE);
        size_t pages_to_free = *(size_t*)raw_page_start;
        
        vmm_free_pages(raw_page_start, pages_to_free);
        return;
    }
    
    // Standard slab free path
    uint32_t page_base = (uint32_t)ptr & ~(PAGE_SIZE - 1);
    struct SlabPage* page = (struct SlabPage*)page_base;
    struct SlabCache* cache = page->owning_cache;
    
    slab_free(cache, ptr);
}

static void* realloc_unlocked(void* ptr, size_t size) {
    if (!ptr) return malloc_unlocked(size);
    
    if (size == 0) {
        free_unlocked(ptr);
        return NULL;
    }
    
    size_t old_size = 0;
    bool is_large_alloc = (((uint32_t)ptr & (PAGE_SIZE - 1)) == 0);
    
    if (is_large_alloc) {
        // Retrieve the total page count from the hidden metadata page (1 page backward)
        void* raw_page_start = (void*)((uint8_t*)ptr - PAGE_SIZE);
        size_t total_pages = *(size_t*)raw_page_start;
        
        // The usable size originally requested was at least:
        // total_size = size + PAGE_SIZE -> num_pages calculation.
        // We can safely derive the absolute maximum byte capacity of this allocation:
        old_size = (total_pages * PAGE_SIZE) - PAGE_SIZE;
        
        // Optimization: If the new size fits within the current page allocation, 
        // we can reuse it without moving memory.
        size_t new_total_size = size + PAGE_SIZE;
        size_t new_num_pages = (new_total_size + PAGE_SIZE - 1) / PAGE_SIZE;
        
        if (new_num_pages == total_pages) {
            return ptr; // Current page block is already large enough
        }
    } else {
        // It's a slab allocation. Find the page base header to figure out the slot size.
        uint32_t page_base = (uint32_t)ptr & ~(PAGE_SIZE - 1);
        struct SlabPage* page = (struct SlabPage*)page_base;
        struct SlabCache* cache = page->owning_cache;
        
        old_size = cache->object_size;
        
        // Optimization: If the new size still fits within this current slab's slot size,
        // and doesn't warrant downsizing to a smaller cache, we can keep it.
        if (size <= old_size) {
            // Optional: If you want to strictly downsize to save space when size is much smaller,
            // you could skip this return. For kernel stability, keeping it is highly efficient.
            
            // Check if it could fit into a smaller slab cache bucket
            bool fits_smaller = false;
            for (int i = 0; i < sizeof(malloc_caches) / sizeof(struct SlabCache); i++) {
                if (size <= malloc_caches[i].object_size) {
                    if (malloc_caches[i].object_size < old_size) {
                        fits_smaller = true;
                    }
                    break;
                }
            }
            // If it doesn't drop down to a smaller slab bucket, reuse the same slot
            if (!fits_smaller) {
                return ptr;
            }
        }
    }
    
    // fallback path: Allocate a completely new block of memory
    void* new_ptr = malloc_unlocked(size);
    if (!new_ptr) {
        return NULL; // Out of memory, original pointer remains valid
    }
    
    // Copy the minimum of the old size and new size
    size_t copy_size = (old_size < size) ? old_size : size;
    memcpy(new_ptr, ptr, copy_size);
    
    // Free the old memory block
    free_unlocked(ptr);
    
    return new_ptr;
}

// Public entry points. The slab lists and page bookkeeping are shared by every
// thread (and the scheduler's reaper runs inside the timer IRQ), so each call
// runs with interrupts disabled.

void* malloc(size_t size) {
    uint32_t flags = irq_save();
    void* ptr = malloc_unlocked(size);
    irq_restore(flags);
    return ptr;
}

void free(void* ptr) {
    if (!ptr) return;
    uint32_t flags = irq_save();
    free_unlocked(ptr);
    irq_restore(flags);
}

void* realloc(void* ptr, size_t size) {
    uint32_t flags = irq_save();
    void* new_ptr = realloc_unlocked(ptr, size);
    irq_restore(flags);
    return new_ptr;
}

