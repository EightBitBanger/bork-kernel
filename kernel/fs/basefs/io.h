#ifndef BASE_IO_H
#define BASE_IO_H

#include <kernel/fs/basefs/structs.h>

// All device I/O goes through ctx->device (a generic BlockDevice). basefs
// keeps a one-sector cache in the window at ctx->device_address, which must
// be at least the device's sector size (see fs_device_window_size).

uint32_t fs_device_window_size(struct BlockDevice* device);

// Addresses are byte offsets on the device (64-bit). The size of a read or
// write is a memory buffer length and stays 32-bit; fs_mem_zero needs no
// buffer, so it takes a 64-bit length.
uint8_t fs_readb(struct FSDeviceContext* ctx, uint64_t address);
void fs_writeb(struct FSDeviceContext* ctx, uint64_t address, uint8_t byte);

void fs_mem_read(struct FSDeviceContext* ctx, uint64_t address, void* destination, uint32_t size);
void fs_mem_write(struct FSDeviceContext* ctx, uint64_t address, const void* source, uint32_t size);

// Write "size" zero bytes starting at "address"
void fs_mem_zero(struct FSDeviceContext* ctx, uint64_t address, uint64_t size);

void fs_cache_sync(struct FSDeviceContext* ctx);

// Write back the cached sector (fs_cache_sync) and then tell the drive to
// commit its own volatile write cache. Returns false if anything failed.
bool fs_cache_flush(struct FSDeviceContext* ctx);

#endif
