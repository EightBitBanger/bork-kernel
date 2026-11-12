#include <kernel/util/string.h>
#include <kernel/malloc.h>

#include <kernel/arch/x86/heap.h>
#include <kernel/arch/x86/irq.h>
#include <kernel/arch/x86/virtual/vmm.h>

//
// kmalloc on top of malloc
//
// Each kmalloc block is a malloc block with a KMallocHeader in front. Live
// blocks are kept on a doubly linked list so kmalloc_next() can enumerate
// them.
//
// Validation: knode code calls kmalloc_is_valid() / kmalloc_get_flags() on
// arbitrary pointers, including raw malloc() pointers that were never
// kmalloc'd. So a header is only trusted when
//   1. both ends of the header are mapped (reading it cannot fault),
//   2. magic matches, and
//   3. check == address ^ KMALLOC_CHECK_KEY, which ties the header to the
//      exact address it was issued for.
// A stray pointer passes 2 and 3 together with probability ~2^-64.
// kfree() clears magic before the block goes back to malloc, so a stale
// pointer stops validating at once (the slab allocator then poisons it).
//
// All list and header updates run with interrupts disabled, the same rule
// malloc() itself follows.

#define KMALLOC_MAGIC        0x4B4D4C43U    // "KMLC"
#define KMALLOC_CHECK_KEY    ((uintptr_t)0xA5C3E1F7U)
#define KMALLOC_HEADER_SIZE  sizeof(struct KMallocHeader)

_Static_assert(sizeof(struct KMallocHeader) % 16 == 0, "KMallocHeader must keep payloads 16-byte aligned");
_Static_assert(sizeof(void*) != 4 || sizeof(struct KMallocHeader) == 32, "KMallocHeader must stay 32 bytes on 32-bit targets");
_Static_assert(offsetof(struct KMallocHeader, size) == 0, "size must be the first header field");

static struct KMallocHeader* kmalloc_head = NULL;
static struct KMallocHeader* kmalloc_tail = NULL;
static uint32_t kmalloc_count = 0;
static uint32_t kmalloc_bytes = 0;

static inline bool page_mapped(uintptr_t address) {
    return vmm_get_phys_addr((void*)address) != 0;
}

static inline void* payload_of(struct KMallocHeader* h) {
    return (uint8_t*)h + KMALLOC_HEADER_SIZE;
}

// Header of a live kmalloc block, or NULL. Interrupts off.
static struct KMallocHeader* header_of(const void* pointer) {
    uintptr_t address = (uintptr_t)pointer;
    if (address < KMALLOC_HEADER_SIZE)
        return NULL;
    if (address & 0xFU)
        return NULL;   // kmalloc payloads are always 16-byte aligned
    
    uintptr_t header_address = address - KMALLOC_HEADER_SIZE;
    
    if (!page_mapped(header_address) || !page_mapped(address - 1))
        return NULL;
    
    struct KMallocHeader* h = (struct KMallocHeader*)header_address;
    if (h->magic != KMALLOC_MAGIC || h->check != (address ^ KMALLOC_CHECK_KEY))
        return NULL;
    return h;
}

static void* kmalloc_unlocked(size_t size) {
    if (size == 0 || size > 0xFFFFFFFFU - KMALLOC_HEADER_SIZE)
        return NULL;
    
    size_t total = size + KMALLOC_HEADER_SIZE;
    uint8_t* block = (uint8_t*)malloc(total);
    if (block == NULL)
        return NULL;
    
    // malloc() hands back slab poison (0xAA); start from zero like a fresh
    // heap would, so partially initialised objects hold no garbage
    memset(block, 0, total);
    
    struct KMallocHeader* h = (struct KMallocHeader*)block;
    void* address = payload_of(h);
    
    h->size  = (uint32_t)size;
    h->magic = KMALLOC_MAGIC;
    h->check = (uintptr_t)address ^ KMALLOC_CHECK_KEY;
    
    // Append, so kmalloc_next() walks in allocation order
    h->prev = kmalloc_tail;
    h->next = NULL;
    if (kmalloc_tail != NULL) kmalloc_tail->next = h;
    else                      kmalloc_head = h;
    kmalloc_tail = h;
    
    kmalloc_count++;
    kmalloc_bytes += (uint32_t)size;
    return address;
}

static void kfree_unlocked(void* address) {
    struct KMallocHeader* h = header_of(address);
    if (h == NULL)
        return;
    
    if (h->prev != NULL) h->prev->next = h->next;
    else                 kmalloc_head = h->next;
    if (h->next != NULL) h->next->prev = h->prev;
    else                 kmalloc_tail = h->prev;
    
    kmalloc_count--;
    kmalloc_bytes -= h->size;
    
    // Invalidate before the memory can be reused
    h->magic = 0;
    h->check = 0;
    
    free(h);
}

void* kmalloc(size_t size) {
    uint32_t flags = irq_save();
    void* address = kmalloc_unlocked(size);
    irq_restore(flags);
    return address;
}

void kfree(void* address) {
    uint32_t flags = irq_save();
    kfree_unlocked(address);
    irq_restore(flags);
}

bool kmalloc_is_valid(const void* address) {
    uint32_t flags = irq_save();
    bool valid = header_of(address) != NULL;
    irq_restore(flags);
    return valid;
}

//
// Attributes
//

#define KMALLOC_GETTER(name, field, type_t)                 \
    type_t name(const void* address) {                      \
        uint32_t irq_flags = irq_save();                    \
        struct KMallocHeader* h = header_of(address);       \
        type_t value = h ? (type_t)h->field : 0;            \
        irq_restore(irq_flags);                             \
        return value;                                       \
    }

#define KMALLOC_SETTER(name, field)                         \
    void name(void* address, uint8_t value) {               \
        uint32_t irq_flags = irq_save();                    \
        struct KMallocHeader* h = header_of(address);       \
        if (h) h->field = value;                            \
        irq_restore(irq_flags);                             \
    }

KMALLOC_GETTER(kmalloc_get_type,        type,  uint8_t)
KMALLOC_GETTER(kmalloc_get_flags,       flags, uint8_t)
KMALLOC_GETTER(kmalloc_get_permissions, perm,  uint8_t)
KMALLOC_GETTER(kmalloc_get_size,        size,  uint32_t)

KMALLOC_SETTER(kmalloc_set_type,        type)
KMALLOC_SETTER(kmalloc_set_flags,       flags)
KMALLOC_SETTER(kmalloc_set_permissions, perm)

//
// Enumeration and stats
//

void* kmalloc_next(const void* previous_address) {
    uint32_t irq_flags = irq_save();
    struct KMallocHeader* next_header;
    
    if (previous_address == NULL) {
        next_header = kmalloc_head;
    } else {
        struct KMallocHeader* h = header_of(previous_address);
        next_header = h ? h->next : NULL;
    }
    
    irq_restore(irq_flags);
    return next_header ? payload_of(next_header) : NULL;
}

uint32_t kmalloc_get_block_count(void) {
    return kmalloc_count;
}

uint32_t kmalloc_get_bytes_in_use(void) {
    return kmalloc_bytes;
}
