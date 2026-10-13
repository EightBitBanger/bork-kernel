
#ifndef _KERNEL_RESOURCE_IO_H_
#define _KERNEL_RESOURCE_IO_H_

// Shared helpers for resource classes: little-endian field encoding and
// exact-length file I/O (vfs_read / vfs_write may return short counts).
// Used by the manager and by every class under resources/classes/.

#include <stdint.h>
#include <stdbool.h>

#include <kernel/vfs/vfs.h>

// ==========================================
// Little-endian encoding
// ==========================================

static inline void resource_put_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static inline void resource_put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint16_t resource_get_u16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t resource_get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ==========================================
// Exact-length file I/O
// ==========================================

// Write exactly `size` bytes at the current position
bool resource_write_exact(File file, const void* buffer, uint32_t size);

// Read exactly `size` bytes from the current position
bool resource_read_exact(File file, void* buffer, uint32_t size);

// Seek to `offset`, then read exactly `size` bytes. Fails if the seek did
// not land on `offset` (past the end of the file).
bool resource_read_at(File file, uint32_t offset, void* buffer, uint32_t size);

#endif
