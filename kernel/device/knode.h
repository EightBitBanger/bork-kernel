#ifndef _KERNEL_DIRECTORY_NODE_H_
#define _KERNEL_DIRECTORY_NODE_H_

#include <stdint.h>

#define KDIRECTORY_REF_MAX    6
#define KDIRECTORYEX_REF_MAX  11

struct KernelDirectoryExtent;

struct KernelDirectory {
    char name[16];
    
    struct KernelDirectory* parent;
    
    struct KernelDirectoryExtent* next;     // First extent
    struct KernelDirectoryExtent* prev;     // Last extent
    
    uint32_t size;
    void* reference[KDIRECTORY_REF_MAX];
};

struct KernelDirectoryExtent {
    struct KernelDirectoryExtent* next;
    struct KernelDirectoryExtent* prev;
    
    uint32_t size;
    void* reference[KDIRECTORYEX_REF_MAX];
};

#endif
