#ifndef _KERNEL_DIRECTORY_NODE_H_
#define _KERNEL_DIRECTORY_NODE_H_

#include <stdint.h>

#define KDIRECTORY_REF_MAX    6
#define KDIRECTORYEX_REF_MAX  11

struct KernelDirectory {
    char name[16];
    
    uint32_t parent;
    
    uint32_t next;
    uint32_t prev;
    
    uint32_t size;
    uint32_t reference[KDIRECTORY_REF_MAX];
};

struct KernelDirectoryExtent {
    uint32_t next;
    uint32_t prev;
    
    uint32_t size;
    uint32_t reference[KDIRECTORYEX_REF_MAX];
};

#endif
