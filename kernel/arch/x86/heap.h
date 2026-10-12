#ifndef KERNEL_MEMORY_HEAP_H
#define KERNEL_MEMORY_HEAP_H

//
// kmalloc: tagged kernel objects
//
// Every memory block comes from malloc() with a small header in front 
// that carries the attributes the knode/device layer relies 
// on (flags, type, permissions, size). Addresses are still handed 
// out as uint32_t and KMALLOC_NULL still means "none", so
// nothing built on top of this API needs to change.
//
//   [ malloc block ............................................ ]
//   [ KMallocHeader (32 bytes) ][ payload (size bytes, zeroed)  ]
//                               ^ address returned by kmalloc()
//
// kmalloc_is_valid() accepts any uint32_t: it only reports true for an
// address kmalloc() returned and kfree() has not released yet. Raw malloc()
// pointers stored in knodes (mount windows, device contexts) are reported as
// invalid, exactly as before, because they never fell inside the old heap.
//

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <kernel/malloc.h>

#define KMALLOC_NULL         0xFFFFFFFFUL

#define KMALLOC_FLAG_DIRTY       0x01
#define KMALLOC_FLAG_MOUNT       0x02
#define KMALLOC_FLAG_DIRECTORY   0x04
#define KMALLOC_FLAG_EXTENT      0x08

#define KMALLOC_PERMISSION_EXECUTABLE  0x01
#define KMALLOC_PERMISSION_READ        0x02
#define KMALLOC_PERMISSION_WRITE       0x04
#define KMALLOC_PERMISSION_USER        0x08

#define KMALLOC_TYPE_DEVICE            0x01
#define KMALLOC_TYPE_DRIVER            0x02
#define KMALLOC_TYPE_BLOCK             0x04
#define KMALLOC_TYPE_RAW               0x08
#define KMALLOC_TYPE_PROCBLOCK         0x10

// size stays the first field: code that reads the header directly
// (destroy_buffer used to) keeps working.
struct __attribute__((aligned(16))) KMallocHeader {
    uint32_t size;          // Payload size requested by the caller
    uint8_t  flags;
    uint8_t  type;
    uint8_t  perm;
    uint8_t  reserved0;
    uint32_t magic;         // KMALLOC_MAGIC while live, 0 once freed
    uint32_t check;         // payload address ^ KMALLOC_CHECK_KEY
    uint32_t prev;          // Header address of the previous live block (0 = none)
    uint32_t next;          // Header address of the next live block (0 = none)
    uint32_t reserved1[2];
};

uint32_t kmalloc(uint32_t size);
void kfree(uint32_t address);

// Walk every live block in allocation order. Pass KMALLOC_NULL to start;
// returns KMALLOC_NULL after the last one.
uint32_t kmalloc_next(uint32_t previous_address);

void kmem_write(uint32_t destination, const void* source, uint32_t size);
void kmem_read(void* destination, uint32_t source, uint32_t size);
void kmemset(uint32_t destination, unsigned char value, uint32_t size);

uint8_t kmalloc_get_flags(uint32_t address);
void    kmalloc_set_flags(uint32_t address, uint8_t flags);

uint8_t kmalloc_get_type(uint32_t address);
void    kmalloc_set_type(uint32_t address, uint8_t type);

uint8_t kmalloc_get_permissions(uint32_t address);
void    kmalloc_set_permissions(uint32_t address, uint8_t type);

uint32_t kmalloc_get_size(uint32_t address);

bool kmalloc_is_valid(uint32_t address);

// Live block count and payload bytes (replaces heap_get_total_memory etc.)
uint32_t kmalloc_get_block_count(void);
uint32_t kmalloc_get_bytes_in_use(void);

#define kmemcpy(arg1, source, sz) _Generic((arg1), \
    uint32_t : kmem_write, \
    default  : kmem_read \
)((arg1), (source), (sz))

#endif
