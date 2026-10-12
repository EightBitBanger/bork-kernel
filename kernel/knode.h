#ifndef _KNODE_DIRECTORY_H_
#define _KNODE_DIRECTORY_H_

#ifdef KERNEL_PLATFORM_X86
  #include <kernel/arch/x86/heap.h>
#endif

#define KNODE_NAME_LENGTH_MAX       16
#define KNODE_NULL                  KMALLOC_NULL

#include <stdint.h>
#include <stdbool.h>

uint32_t create_knode(const char* name, uint32_t parent_address);
bool destroy_knode(uint32_t address, uint32_t parent_address);

void knode_get_name(uint32_t address, char* name);
void knode_set_name(uint32_t address, const char* name);

void knode_get_permissions(uint32_t address, uint8_t* permissions);
void knode_set_permissions(uint32_t address, uint8_t permissions);

uint32_t knode_get_root(void);
void knode_set_root(uint32_t address);

uint8_t knode_add_reference(uint32_t directory_address, uint32_t reference_ptr);
uint8_t knode_remove_reference(uint32_t directory_address, uint32_t reference_ptr);
uint32_t knode_get_reference(uint32_t directory_address, uint32_t index);
uint32_t knode_get_reference_count(uint32_t directory_address);
uint32_t knode_find_by_name(uint32_t directory_address, const char* name);

uint32_t create_extent(void);
void     destroy_extent(uint32_t address);

uint32_t knode_get_parent(uint32_t directory_address);
uint32_t knode_get_root(void);

bool knode_check_is_valid(uint32_t address);

#endif

