
#include <stdint.h>
#include <stdbool.h>

#include <kernel/block/block.h>
#include <kernel/fs/config.h>
#include <kernel/fs/basefs/io.h>
#include <kernel/fs/basefs/structs.h>
#include <kernel/util/string.h>

// basefs I/O on top of the generic block layer. Each context carries its
// own BlockDevice and a one-sector cache window at ctx->device_address.
//
// fs_mem_read / fs_mem_write move whole sector-sized pieces with memcpy,
// and a write that covers a whole sector does not read that sector from the
// disk first.

#define FS_NO_SECTOR 0xFFFFFFFFFFFFFFFFULL

uint32_t fs_device_window_size(struct BlockDevice* device) {
    return (device != NULL) ? device->sector_size : 0;
}

static inline uint32_t fs_io_sector_size(struct FSDeviceContext* ctx) {
    return ctx->device->sector_size;
}

static inline uint8_t* fs_io_window(struct FSDeviceContext* ctx) {
    return ctx->device_address;
}

static inline bool fs_io_ready(struct FSDeviceContext* ctx) {
    return ctx && ctx->device && ctx->device_address != NULL && ctx->device->sector_size != 0;
}

// Write the cached sector back if it was modified
static bool fs_cache_write_back(struct FSDeviceContext* ctx) {
    if (!ctx->sector_dirty || ctx->sector_frame == FS_NO_SECTOR)
        return true;

    if (!block_write(ctx->device, ctx->sector_frame, 1, fs_io_window(ctx)))
        return false;

    ctx->sector_dirty = false;
    return true;
}

static bool fs_cache_flush_and_fetch(struct FSDeviceContext* ctx, uint64_t target_sector) {
    if (!fs_io_ready(ctx))
        return false;
    if (ctx->sector_frame == target_sector)
        return true;

    if (!fs_cache_write_back(ctx))
        return false;

    if (!block_read(ctx->device, target_sector, 1, fs_io_window(ctx))) {
        // The window may now hold a partial sector: forget what it held
        ctx->sector_frame = FS_NO_SECTOR;
        return false;
    }

    ctx->sector_frame = target_sector;
    return true;
}

// Make the window hold `target_sector` WITHOUT reading it, because the
// caller is about to overwrite every byte of it.
static bool fs_cache_claim(struct FSDeviceContext* ctx, uint64_t target_sector) {
    if (!fs_io_ready(ctx))
        return false;
    if (ctx->sector_frame == target_sector)
        return true;
    if (!fs_cache_write_back(ctx))
        return false;
    ctx->sector_frame = target_sector;
    return true;
}

void fs_cache_sync(struct FSDeviceContext* ctx) {
    if (!ctx || !ctx->device) return;
    fs_cache_write_back(ctx);
}

bool fs_cache_flush(struct FSDeviceContext* ctx) {
    if (!ctx || !ctx->device) return false;

    fs_cache_sync(ctx);
    if (ctx->sector_dirty)
        return false;   // The write-back above failed

    return block_flush(ctx->device);
}

uint8_t fs_readb(struct FSDeviceContext* ctx, uint64_t address) {
    if (!fs_io_ready(ctx)) return 0;
    uint32_t sector_size   = fs_io_sector_size(ctx);
    uint64_t target_sector = address / sector_size;
    uint32_t byte_offset   = (uint32_t)(address % sector_size);

    if (!fs_cache_flush_and_fetch(ctx, target_sector))
        return 0;

    return fs_io_window(ctx)[byte_offset];
}

void fs_writeb(struct FSDeviceContext* ctx, uint64_t address, uint8_t byte) {
    if (!fs_io_ready(ctx)) return;
    uint32_t sector_size   = fs_io_sector_size(ctx);
    uint64_t target_sector = address / sector_size;
    uint32_t byte_offset   = (uint32_t)(address % sector_size);

    if (!fs_cache_flush_and_fetch(ctx, target_sector))
        return;

    fs_io_window(ctx)[byte_offset] = byte;
    ctx->sector_dirty = true;
}

#if FS_IO_DIRECT_TRANSFER
// Number of whole sectors starting at `sector` that can bypass the window
static uint32_t fs_io_burst_length(uint64_t size, uint32_t sector_size) {
    uint64_t count = size / sector_size;
    if (count > FS_IO_MAX_BURST_SECTORS)
        count = FS_IO_MAX_BURST_SECTORS;
    return (uint32_t)count;
}

static inline bool fs_io_cached_in(struct FSDeviceContext* ctx, uint64_t first, uint32_t count) {
    return ctx->sector_frame != FS_NO_SECTOR &&
           ctx->sector_frame >= first && ctx->sector_frame - first < count;
}
#endif

void fs_mem_read(struct FSDeviceContext* ctx, uint64_t address, void* destination, uint32_t size) {
    if (!ctx || destination == NULL) return;
    uint8_t* dest_ptr = (uint8_t*)destination;
    if (!fs_io_ready(ctx)) {
        memset(dest_ptr, 0, size);
        return;
    }
    uint32_t sector_size = fs_io_sector_size(ctx);

    while (size > 0) {
        uint64_t sector = address / sector_size;
        uint32_t offset = (uint32_t)(address % sector_size);

#if FS_IO_DIRECT_TRANSFER
        if (offset == 0 && size >= sector_size) {
            uint32_t count = fs_io_burst_length(size, sector_size);
            // The disk copy of a dirty cached sector is stale: write it first
            if (fs_io_cached_in(ctx, sector, count) && !fs_cache_write_back(ctx))
                break;
            if (!block_read(ctx->device, sector, count, dest_ptr))
                break;
            uint32_t bytes = count * sector_size;
            dest_ptr += bytes;
            address  += bytes;
            size     -= bytes;
            continue;
        }
#endif

        uint32_t chunk = sector_size - offset;
        if (chunk > size)
            chunk = size;
        if (!fs_cache_flush_and_fetch(ctx, sector))
            break;
        memcpy(dest_ptr, fs_io_window(ctx) + offset, chunk);
        dest_ptr += chunk;
        address  += chunk;
        size     -= chunk;
    }

    // Unreadable bytes come back as 0
    if (size > 0)
        memset(dest_ptr, 0, size);
}

// Shared body of fs_mem_write and fs_mem_zero (source == NULL writes zeros)
static void fs_mem_store(struct FSDeviceContext* ctx, uint64_t address, const uint8_t* source, uint64_t size) {
    if (!fs_io_ready(ctx)) return;
    uint32_t sector_size = fs_io_sector_size(ctx);

    while (size > 0) {
        uint64_t sector = address / sector_size;
        uint32_t offset = (uint32_t)(address % sector_size);

#if FS_IO_DIRECT_TRANSFER
        if (source != NULL && offset == 0 && size >= sector_size) {
            uint32_t count = fs_io_burst_length(size, sector_size);
            if (!block_write(ctx->device, sector, count, source))
                return;
            // Every byte of a cached sector in this range was just replaced
            if (fs_io_cached_in(ctx, sector, count)) {
                ctx->sector_frame = FS_NO_SECTOR;
                ctx->sector_dirty = false;
            }
            uint32_t bytes = count * sector_size;
            source  += bytes;
            address += bytes;
            size    -= bytes;
            continue;
        }
#endif

        uint32_t chunk = sector_size - offset;
        if (chunk > size)
            chunk = (uint32_t)size;

        bool ok = (chunk == sector_size) ? fs_cache_claim(ctx, sector)
                                         : fs_cache_flush_and_fetch(ctx, sector);
        if (!ok)
            return;

        if (source != NULL) {
            memcpy(fs_io_window(ctx) + offset, source, chunk);
            source += chunk;
        } else {
            memset(fs_io_window(ctx) + offset, 0, chunk);
        }
        ctx->sector_dirty = true;
        address += chunk;
        size    -= chunk;
    }
}

void fs_mem_write(struct FSDeviceContext* ctx, uint64_t address, const void* source, uint32_t size) {
    if (!ctx || source == NULL) return;
    fs_mem_store(ctx, address, (const uint8_t*)source, size);
}

void fs_mem_zero(struct FSDeviceContext* ctx, uint64_t address, uint64_t size) {
    if (!ctx) return;
    fs_mem_store(ctx, address, NULL, size);
}
