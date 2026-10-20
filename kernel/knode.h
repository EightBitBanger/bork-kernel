#ifndef _KNODE_DIRECTORY_H_
#define _KNODE_DIRECTORY_H_

#ifdef KERNEL_PLATFORM_X86
  #include <kernel/arch/x86/heap.h>
#endif

#define KNODE_NAME_LENGTH_MAX       16

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Knodes and everything they reference are plain kernel pointers (NULL =
// none). A reference may be another knode, a kmalloc'd value (device,
// process record) or a raw malloc() block owned by a mount.

void* create_knode(const char* name, void* parent_address);
bool destroy_knode(void* address, void* parent_address);

void knode_get_name(const void* address, char* name);
void knode_set_name(void* address, const char* name);

void knode_get_permissions(const void* address, uint8_t* permissions);
void knode_set_permissions(void* address, uint8_t permissions);

void* knode_get_root(void);
void knode_set_root(void* address);

uint8_t knode_add_reference(void* directory_address, void* reference_ptr);
uint8_t knode_remove_reference(void* directory_address, const void* reference_ptr);
void* knode_get_reference(const void* directory_address, uint32_t index);
uint32_t knode_get_reference_count(const void* directory_address);
void* knode_find_by_name(const void* directory_address, const char* name);

void* create_extent(void);
void  destroy_extent(void* address);

void* knode_get_parent(void* directory_address);

bool knode_check_is_valid(const void* address);

#endif
