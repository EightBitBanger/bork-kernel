
#ifndef _KERNEL_RESOURCE_CLASS_H_
#define _KERNEL_RESOURCE_CLASS_H_

// The interface a resource class implements so the manager can store it in
// a resource file. Each class lives in resources/classes/ and exports one
// `const ResourceHandler`, which the manager lists in its handler table.
//
// The manager owns the 16-byte file header; a class only ever sees its own
// payload. When `write` or `read` is called, the file is positioned at the
// first payload byte.

#include <stdint.h>
#include <stdbool.h>

#include <kernel/vfs/vfs.h>

typedef struct ResourceHandler {
    const char* name;

    // Exact payload size in bytes, or 0 if the resource can't be stored
    uint32_t (*payload_size)(const void* resource);

    // Write the payload at the current file position
    bool (*write)(File file, const void* resource);

    // Read and validate payload_size bytes, returning a heap-allocated resource
    void* (*read)(File file, uint32_t payload_size);

    // Free a resource returned by read (NULL: plain free())
    void (*release)(void* resource);
} ResourceHandler;

#endif
