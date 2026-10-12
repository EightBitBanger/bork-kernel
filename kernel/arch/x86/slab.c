#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/arch/x86/slab.h>
#include <kernel/panic/panic_error.h>
#include <kernel/util/string.h>

struct FreeNode {
    struct FreeNode* next;
};

static struct SlabPage* slab_grow(size_t object_size) {
    // Get a fresh page from the virtual memory manager
    void* raw_page = vmm_alloc_pages(1);
    if (!raw_page) return NULL;
    
    memset(raw_page, 0, PAGE_SIZE);
    
    struct SlabPage* page = (struct SlabPage*)raw_page;
    page->num_allocated = 0;
    page->next = NULL;
    
    uint32_t header_size = sizeof(struct SlabPage);
    uint32_t start_offset = (header_size + 3) & ~3U;
    
    uint8_t* page_bytes = (uint8_t*)raw_page;
    uint8_t* first_slot = page_bytes + start_offset;
    
    if (object_size < sizeof(struct FreeNode)) {
        object_size = sizeof(struct FreeNode);
    }
    
    page->free_list = (struct FreeNode*)first_slot;
    struct FreeNode* current = page->free_list;
    
    uint32_t max_slots = (PAGE_SIZE - start_offset) / object_size;
    
    for (uint32_t i = 0; i < max_slots - 1; i++) {
        struct FreeNode* next_node = (struct FreeNode*)((uint8_t*)current + object_size);
        current->next = next_node;
        
        // Skip the first 4 bytes to maintain list structure
        uint8_t* payload = (uint8_t*)current + sizeof(struct FreeNode*);
        size_t payload_size = object_size - sizeof(struct FreeNode*);
        memset(payload, 0xAA, payload_size);
        
        current = next_node;
    }
    
    // Terminate the last node and poison its payload
    current->next = NULL;
    uint8_t* payload = (uint8_t*)current + sizeof(struct FreeNode*);
    size_t payload_size = object_size - sizeof(struct FreeNode*);
    memset(payload, 0xAA, payload_size);
    
    return page;
}

void* slab_alloc(struct SlabCache* cache) {
    struct SlabPage* page = cache->page_list;
    
    while (page != NULL && page->free_list == NULL) {
        page = page->next;
    }
    
    if (!page) {
        page = slab_grow(cache->object_size);
        if (!page) return NULL;
        
        page->next = cache->page_list;
        page->owning_cache = cache;
        cache->page_list = page;
    }
    
    struct FreeNode* allocated_slot = page->free_list;
    
    // Make sure we check if the object size is larger than a pointer
    if (cache->object_size > sizeof(struct FreeNode*)) {
        uint8_t* payload = (uint8_t*)allocated_slot + sizeof(struct FreeNode*);
        size_t payload_size = cache->object_size - sizeof(struct FreeNode*);
        
        for (size_t i = 0; i < payload_size; i++) {
            if (payload[i] != 0xAA) {
                kernel_crashout(0x00, (uint32_t)allocated_slot, PT_SEG_FAULT, "SEGMENTATION FAULT");
                return NULL;
            }
        }
    }
    
    // Safe to pop from list if it passed the check
    page->free_list = allocated_slot->next;
    page->num_allocated++;
    
    return (void*)allocated_slot;
}

void slab_free(struct SlabCache* cache, void* ptr) {
    if (!ptr) return;
    
    // Find the base address of the 4KB page containing this pointer
    uint32_t page_base = (uint32_t)ptr & ~(PAGE_SIZE - 1);
    struct SlabPage* page = (struct SlabPage*)page_base;
    
    // Check double free
    struct FreeNode* current = page->free_list;
    while (current != NULL) {
        if (current == (struct FreeNode*)ptr) {
            
            kernel_crashout(0x00, (uint32_t)ptr, PT_SEG_FAULT, "double Free");
            return;
        }
        current = current->next;
    }
    
    // Poison the old user data
    memset(ptr, 0xAA, cache->object_size);
    
    // Link back into the free list
    struct FreeNode* node = (struct FreeNode*)ptr;
    node->next = page->free_list;
    page->free_list = node;
    page->num_allocated--;
    
    if (page->num_allocated == 0) {
        // Unlink page from cache->page_list
        struct SlabPage** prev = &cache->page_list;
        while (*prev != NULL && *prev != page) {
            prev = &(*prev)->next;
        }
        if (*prev == page) {
            *prev = page->next;
        }
        
        // Unmap the page and return the physical frame
        vmm_free_pages((void*)page, 1); 
    }
}
