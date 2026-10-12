#ifndef _KERNEL_MEMORY_SLAB_ALLOCATOR_H_
#define _KERNEL_MEMORY_SLAB_ALLOCATOR_H_

struct SlabCache {
    size_t object_size;                // Size of objects this cache handles
    struct SlabPage* page_list;        // Linked list of all pages in this cache
};

struct SlabPage {
    struct FreeNode* free_list;        // Points to the first available slot in this page
    size_t num_allocated;              // How many slots are currently active
    struct SlabPage* next;             // Link to the next page in the cache
    struct SlabCache* owning_cache;    // Pointer back to the original cache
};

void* slab_alloc(struct SlabCache* cache);

void slab_free(struct SlabCache* cache, void* ptr);

#endif
