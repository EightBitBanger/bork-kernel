#include <kernel/util/string.h>
#include <kernel/malloc.h>

#include <kernel/arch/x86/heap.h>
#include <kernel/arch/x86/irq.h>
#include <kernel/arch/x86/virtual/vmm.h>

//
// kmalloc on top of malloc
//
// The old fixed 4 MB bitmap heap is gone. Each kmalloc block is a malloc
// block with a KMallocHeader in front. Live blocks are kept on a doubly
// linked list so kmalloc_next() can still enumerate them.
//
// Validation: knode code calls kmalloc_is_valid() / kmalloc_get_flags() on
// arbitrary uint32_t values, including raw malloc() pointers that were never
// kmalloc'd. So a header is only trusted when
//   1. both ends of the header are mapped (reading it cannot fault),
//   2. magic matches, and
//   3. check == address ^ KMALLOC_CHECK_KEY, which ties the header to the
//      exact address it was issued for.
// A stray pointer passes 2 and 3 together with probability ~2^-64.
// kfree() clears magic before the block goes back to malloc, so a stale
// address stops validating at once (the slab allocator then poisons it).
//
// All list and header updates run with interrupts disabled, the same rule
// the old heap and malloc() itself follow.

#define KMALLOC_MAGIC        0x4B4D4C43U    // "KMLC"
#define KMALLOC_CHECK_KEY    0xA5C3E1F7U
#define KMALLOC_HEADER_SIZE  ((uint32_t)sizeof(struct KMallocHeader))

_Static_assert(sizeof(struct KMallocHeader) == 32, "KMallocHeader must stay 32 bytes (keeps payloads 16-byte aligned)");
_Static_assert(offsetof(struct KMallocHeader, size) == 0, "size must be the first header field");

static uint32_t kmalloc_head  = 0;   // Header addresses, 0 = empty
static uint32_t kmalloc_tail  = 0;
static uint32_t kmalloc_count = 0;
static uint32_t kmalloc_bytes = 0;

static inline struct KMallocHeader* hdr(uint32_t header_address) {
    return (struct KMallocHeader*)(uintptr_t)header_address;
}

static inline bool page_mapped(uint32_t address) {
    return vmm_get_phys_addr((void*)(uintptr_t)address) != 0;
}

// Header of a live kmalloc block, or NULL. Interrupts off.
static struct KMallocHeader* header_of(uint32_t address) {
    if (address == KMALLOC_NULL || address < KMALLOC_HEADER_SIZE)
        return NULL;
    if (address & 0xFU)
        return NULL;   // kmalloc payloads are always 16-byte aligned
    
    uint32_t header_address = address - KMALLOC_HEADER_SIZE;
    
    if (!page_mapped(header_address) || !page_mapped(address - 1))
        return NULL;
    
    struct KMallocHeader* h = hdr(header_address);
    if (h->magic != KMALLOC_MAGIC || h->check != (address ^ KMALLOC_CHECK_KEY))
        return NULL;
    return h;
}

static uint32_t kmalloc_unlocked(uint32_t size) {
    if (size == 0 || size > 0xFFFFFFFFU - KMALLOC_HEADER_SIZE)
        return KMALLOC_NULL;
    
    uint32_t total = size + KMALLOC_HEADER_SIZE;
    uint8_t* block = (uint8_t*)malloc(total);
    if (block == NULL)
        return KMALLOC_NULL;
    
    // malloc() hands back slab poison (0xAA); start from zero like a fresh
    // heap would, so partially initialised objects hold no garbage
    memset(block, 0, total);
    
    uint32_t header_address = (uint32_t)(uintptr_t)block;
    uint32_t address        = header_address + KMALLOC_HEADER_SIZE;
    
    struct KMallocHeader* h = hdr(header_address);
    h->size  = size;
    h->magic = KMALLOC_MAGIC;
    h->check = address ^ KMALLOC_CHECK_KEY;
    
    // Append, so kmalloc_next() walks in allocation order
    h->prev = kmalloc_tail;
    h->next = 0;
    if (kmalloc_tail != 0) hdr(kmalloc_tail)->next = header_address;
    else                   kmalloc_head = header_address;
    kmalloc_tail = header_address;
    
    kmalloc_count++;
    kmalloc_bytes += size;
    return address;
}

static void kfree_unlocked(uint32_t address) {
    struct KMallocHeader* h = header_of(address);
    if (h == NULL)
        return;
    
    uint32_t header_address = address - KMALLOC_HEADER_SIZE;
    
    if (h->prev != 0) hdr(h->prev)->next = h->next;
    else              kmalloc_head = h->next;
    if (h->next != 0) hdr(h->next)->prev = h->prev;
    else              kmalloc_tail = h->prev;
    
    kmalloc_count--;
    kmalloc_bytes -= h->size;
    
    // Invalidate before the memory can be reused
    h->magic = 0;
    h->check = 0;
    
    free((void*)(uintptr_t)header_address);
}

uint32_t kmalloc(uint32_t size) {
    uint32_t flags = irq_save();
    uint32_t address = kmalloc_unlocked(size);
    irq_restore(flags);
    return address;
}

void kfree(uint32_t address) {
    uint32_t flags = irq_save();
    kfree_unlocked(address);
    irq_restore(flags);
}

bool kmalloc_is_valid(uint32_t address) {
    uint32_t flags = irq_save();
    bool valid = header_of(address) != NULL;
    irq_restore(flags);
    return valid;
}

//
// Attributes
//

#define KMALLOC_GETTER(name, field, type_t)                 \
    type_t name(uint32_t address) {                         \
        uint32_t irq_flags = irq_save();                    \
        struct KMallocHeader* h = header_of(address);       \
        type_t value = h ? (type_t)h->field : 0;            \
        irq_restore(irq_flags);                             \
        return value;                                       \
    }

#define KMALLOC_SETTER(name, field)                         \
    void name(uint32_t address, uint8_t value) {            \
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

uint32_t kmalloc_next(uint32_t previous_address) {
    uint32_t irq_flags = irq_save();
    uint32_t next_header;
    
    if (previous_address == KMALLOC_NULL) {
        next_header = kmalloc_head;
    } else {
        struct KMallocHeader* h = header_of(previous_address);
        next_header = h ? h->next : 0;
    }
    
    irq_restore(irq_flags);
    return next_header ? next_header + KMALLOC_HEADER_SIZE : KMALLOC_NULL;
}

uint32_t kmalloc_get_block_count(void) {
    return kmalloc_count;
}

uint32_t kmalloc_get_bytes_in_use(void) {
    return kmalloc_bytes;
}

//
// Raw access (addresses are plain kernel virtual addresses)
//

void kmem_write(uint32_t destination, const void* source, uint32_t size) {
    memcpy((void*)(uintptr_t)destination, source, size);
}

void kmem_read(void* destination, uint32_t source, uint32_t size) {
    memcpy(destination, (const void*)(uintptr_t)source, size);
}

void kmemset(uint32_t destination, unsigned char value, uint32_t size) {
    memset((void*)(uintptr_t)destination, value, size);
}
