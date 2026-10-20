#ifndef KERNEL_MEMORY_HEAP_H
#define KERNEL_MEMORY_HEAP_H

//
// kmalloc: tagged kernel objects
//
// Every memory block comes from malloc() with a small header in front 
// that carries the attributes the knode/device layer relies 
// on (flags, type, permissions, size). Blocks are handed out as
// plain pointers and NULL means "none"
//
//   [ malloc block ............................................ ]
//   [ KMallocHeader (32 bytes) ][ payload (size bytes, zeroed)  ]
//                               ^ pointer returned by kmalloc()
//
// kmalloc_is_valid() accepts any pointer: it only reports true for one
// kmalloc() returned and kfree() has not released yet. Raw malloc()
// pointers stored in knodes (mount windows, device contexts) are reported as
// invalid because they carry no valid header.
//

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <kernel/malloc.h>

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

// size stays the first field, for code that reads the header directly.
// 32 bytes on x86; wider pointers make it 48, still a multiple of 16.
struct __attribute__((aligned(16))) KMallocHeader {
    uint32_t size;          // Payload size requested by the caller
    uint8_t  flags;
    uint8_t  type;
    uint8_t  perm;
    uint8_t  reserved0;
    uint32_t magic;         // KMALLOC_MAGIC while live, 0 once freed
    uintptr_t check;        // payload address ^ KMALLOC_CHECK_KEY
    struct KMallocHeader* prev;     // Previous live block (NULL = none)
    struct KMallocHeader* next;     // Next live block (NULL = none)
    uint32_t reserved1[2];
};

void* kmalloc(size_t size);
void kfree(void* address);

// Walk every live block in allocation order. Pass NULL to start;
// returns NULL after the last one.
void* kmalloc_next(const void* previous_address);

uint8_t kmalloc_get_flags(const void* address);
void    kmalloc_set_flags(void* address, uint8_t flags);

uint8_t kmalloc_get_type(const void* address);
void    kmalloc_set_type(void* address, uint8_t type);

uint8_t kmalloc_get_permissions(const void* address);
void    kmalloc_set_permissions(void* address, uint8_t type);

uint32_t kmalloc_get_size(const void* address);

bool kmalloc_is_valid(const void* address);

// Live block count and payload bytes
uint32_t kmalloc_get_block_count(void);
uint32_t kmalloc_get_bytes_in_use(void);

#endif
