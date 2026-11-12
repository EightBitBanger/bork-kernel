#include <kernel/fs/fs.h>
#include <kernel/block/block.h>
#include <kernel/util/string.h>

// Clock for created/modified stamps. NULL until the kernel provides one.
static uint32_t (*fs_clock)(void) = NULL;

void fs_set_time_source(uint32_t (*now)(void)) {
    fs_clock = now;
}

uint32_t fs_time_now(void) {
    return (fs_clock != NULL) ? fs_clock() : 0;
}

// Block sizes basefs can work with: big enough for a directory header
// plus the longest record, and keeping records 4-byte aligned
static inline bool fs_block_size_valid(uint32_t sector_size) {
    return sector_size >= FS_MIN_SECTOR_SIZE && (sector_size % 4U) == 0;
}

uint8_t fs_device_get_partition(struct FSDeviceContext* device_context, struct FSPartitionBlock* part) {
    if (!device_context) return 1;
    uint64_t offset = sizeof(struct FSDeviceHeader);
    fs_mem_read(device_context, offset, part, sizeof(struct FSPartitionBlock));
    if (part->magic != FS_MAGIC) return 1;
    if (part->version != FS_FORMAT_VERSION) return 1;   // Older (or newer) format
    return 0;
}

bool fs_device_probe(const uint8_t* sector0, uint32_t size) {
    if (sector0 == NULL || size < sizeof(struct FSDeviceHeader) + sizeof(struct FSPartitionBlock))
        return false;
    
    struct FSDeviceHeader header;
    struct FSPartitionBlock part;
    memcpy(&header, sector0, sizeof(header));
    memcpy(&part, sector0 + sizeof(header), sizeof(part));
    
    return header.id == FS_DEVICE_ID && part.magic == FS_MAGIC && part.version == FS_FORMAT_VERSION &&
           fs_block_size_valid(part.sector_size);
}

struct FSDeviceContext fs_device_open(struct BlockDevice* device, uint8_t* window_address, struct FSPartitionBlock* partition) {
    struct FSDeviceContext new_context;
    memset(&new_context, 0x00, sizeof(struct FSDeviceContext));
    
    // Device and window are recorded even when the device turns out to be
    // unformatted, so the context can still be handed to fs_device_format
    new_context.device         = device;
    new_context.device_address = window_address;
    new_context.frame_offset   = FS_INVALID_FRAME;
    new_context.sector_frame   = FS_INVALID_FRAME;
    new_context.alloc_hint     = 0;     // fs_alloc treats this as "start of pool"
    
    if (device == NULL || window_address == NULL)
        return new_context;
    
    if (fs_device_get_partition(&new_context, partition) != 0) 
        return new_context;
    
    // Reject a partition that claims more space than the device has, or a
    // nonsense block size: both would send reads past the end of the disk
    if (!fs_block_size_valid(partition->sector_size) || partition->total_size < partition->sector_size ||
        partition->total_size > block_capacity_bytes(device))
        return new_context;
    
    new_context.is_open          = true;
    new_context.sector_size      = partition->sector_size;
    new_context.pool_size        = partition->total_size;
    new_context.block_count      = new_context.pool_size / new_context.sector_size;
    new_context.bitmap_size      = (new_context.block_count + 7ULL) / 8ULL;
    uint64_t meta_sz = sizeof(struct FSDeviceHeader) + sizeof(struct FSPartitionBlock) + new_context.bitmap_size;
    new_context.reserved_blocks  = (meta_sz + new_context.sector_size - 1ULL) / new_context.sector_size;
    new_context.frame_dirty      = false;
    new_context.sector_dirty     = false;
    
    return new_context;
}

// Bitmap bytes one fs_used_bytes_step counts: 8 sectors of 512 bytes, so a
// step is a short, bounded amount of disk I/O
#define FS_USED_STEP_BYTES  4096U
#define FS_USED_READ_BYTES  256U     // Read buffer on the stack

static inline uint32_t fs_bits_set(uint8_t byte) {
    byte = (uint8_t)(byte - ((byte >> 1) & 0x55U));
    byte = (uint8_t)((byte & 0x33U) + ((byte >> 2) & 0x33U));
    return (uint32_t)((byte + (byte >> 4)) & 0x0FU);
}

static uint64_t fs_used_from_count(struct FSDeviceContext* ctx) {
    uint64_t bytes = ctx->used_blocks * ctx->sector_size;
    return (bytes > ctx->pool_size) ? ctx->pool_size : bytes;
}

// The bytes in use are the set bits of the allocation bitmap (the reserved
// metadata blocks are set at format) times the block size. Counting them
// once means reading the whole bitmap, which on a large stick is thousands
// of sectors, so it is done FS_USED_STEP_BYTES at a time: each call counts
// one more piece and returns false until the count reaches the end, then
// true with *used. After that the count is kept in memory (see
// fs_bitmap_set / _clear) and every call answers at once. `percent` (may be
// NULL) gets how much of the bitmap has been counted.
bool fs_used_bytes_step(struct FSDeviceContext* ctx, uint64_t* used, uint32_t* percent) {
    *used = 0;
    if (percent) *percent = 100;
    if (!ctx || !ctx->is_open || ctx->pool_size == 0 || ctx->sector_size == 0)
        return true;
    
    if (ctx->used_scan < ctx->block_count) {
        // The cached bitmap frame may hold changes the disk doesn't have yet
        fs_bitmap_flush(ctx);
        
        uint64_t bitmap = sizeof(struct FSDeviceHeader) + sizeof(struct FSPartitionBlock);
        uint64_t byte   = ctx->used_scan >> 3;
        uint64_t end    = (ctx->block_count + 7ULL) >> 3;
        if (end - byte > FS_USED_STEP_BYTES)
            end = byte + FS_USED_STEP_BYTES;
        
        uint8_t buffer[FS_USED_READ_BYTES];
        while (byte < end) {
            uint32_t n = (end - byte > FS_USED_READ_BYTES) ? FS_USED_READ_BYTES : (uint32_t)(end - byte);
            fs_mem_read(ctx, bitmap + byte, buffer, n);
            for (uint32_t i = 0; i < n; i++, byte++) {
                uint8_t bits = buffer[i];
                uint64_t first = byte << 3;
                if (first + 8ULL > ctx->block_count)       // Past the last block: not blocks
                    bits &= (uint8_t)((1U << (ctx->block_count - first)) - 1U);
                ctx->used_blocks += fs_bits_set(bits);
            }
            ctx->used_scan = byte << 3;
        }
        if (ctx->used_scan < ctx->block_count) {
            if (percent) *percent = (uint32_t)(ctx->used_scan / (ctx->block_count / 100ULL + 1ULL));
            if (percent && *percent > 99) *percent = 99;
            return false;
        }
        ctx->used_scan = ctx->block_count;
    }
    
    *used = fs_used_from_count(ctx);
    return true;
}

uint64_t fs_get_used_bytes(struct FSDeviceContext* device_context) {
    uint64_t used = 0;
    while (!fs_used_bytes_step(device_context, &used, NULL))
        ;
    return used;
}

// Callers keep size <= pool_size, so this cannot overflow
static inline uint64_t fs_alloc_blocks_for(struct FSDeviceContext* ctx, uint64_t size) {
    return ((uint64_t)sizeof(struct FSAllocHeader) + size + ctx->sector_size - 1ULL) / ctx->sector_size;
}

// First run of `needed` free blocks in [from, to), or FS_NULL. Whole bitmap
// bytes that are full (or empty) are stepped over 8 blocks at a time.
static uint64_t fs_alloc_scan(struct FSDeviceContext* ctx, uint64_t from, uint64_t to, uint64_t needed) {
    uint64_t run_start = 0, run_length = 0;
    uint64_t i = from;
    while (i < to) {
        if ((i & 7U) == 0 && to - i >= 8) {
            uint8_t byte = fs_bitmap_get_byte(ctx, i >> 3);
            if (byte == 0xFF) {
                run_length = 0;
                i += 8;
                continue;
            }
            if (byte == 0x00) {
                if (run_length == 0) run_start = i;
                run_length += 8;
                if (run_length >= needed) return run_start;
                i += 8;
                continue;
            }
        }
        if (!fs_bitmap_get(ctx, i)) {
            if (run_length == 0) run_start = i;
            if (++run_length >= needed) return run_start;
        } else {
            run_length = 0;
        }
        i++;
    }
    return FS_NULL;
}

uint64_t fs_alloc(struct FSDeviceContext* device_context, uint64_t size) {
    if (!device_context || device_context->sector_size == 0) return FS_NULL;
    if (size > device_context->pool_size) return FS_NULL;
    struct FSAllocHeader header;
    uint64_t blocks_needed = fs_alloc_blocks_for(device_context, size);
    uint64_t first = device_context->reserved_blocks;
    uint64_t last  = device_context->block_count;
    
    // Next-fit: carry on from where the previous allocation ended instead of
    // rescanning every used block from the start of the disk each time, then
    // wrap around once.
    uint64_t hint = device_context->alloc_hint;
    if (hint < first || hint >= last)
        hint = first;
    
    uint64_t run_start = fs_alloc_scan(device_context, hint, last, blocks_needed);
    if (run_start == FS_NULL && hint > first) {
        uint64_t wrap_end = hint + blocks_needed - 1U;   // Runs may straddle the hint
        if (wrap_end > last || wrap_end < hint) wrap_end = last;
        run_start = fs_alloc_scan(device_context, first, wrap_end, blocks_needed);
    }
    if (run_start == FS_NULL)
        return FS_NULL;
    
    for (uint64_t j = run_start; j < run_start + blocks_needed; j++) {
        fs_bitmap_set(device_context, j);
    }
    fs_bitmap_flush(device_context);
    device_context->alloc_hint = run_start + blocks_needed;
    
    uint64_t addr = run_start * device_context->sector_size;
    header.size = size;
    fs_mem_write(device_context, addr, &header, sizeof(struct FSAllocHeader));
    return addr + sizeof(struct FSAllocHeader);
}

bool fs_alloc_resize(struct FSDeviceContext* device_context, uint64_t address, uint64_t new_size) {
    if (!device_context || device_context->sector_size == 0) return false;
    if (address == FS_NULL || address < sizeof(struct FSAllocHeader)) return false;
    if (new_size > device_context->pool_size) return false;
    
    uint64_t alloc_addr = address - sizeof(struct FSAllocHeader);
    struct FSAllocHeader header;
    fs_mem_read(device_context, alloc_addr, &header, sizeof(struct FSAllocHeader));
    if (header.size == 0 || header.size > device_context->pool_size)
        return false;   // Not a live allocation
    
    uint64_t start_block = alloc_addr / device_context->sector_size;
    uint64_t old_blocks  = fs_alloc_blocks_for(device_context, header.size);
    uint64_t new_blocks  = fs_alloc_blocks_for(device_context, new_size);
    
    if (new_blocks > old_blocks) {
        if (start_block + new_blocks > device_context->block_count)
            return false;
        for (uint64_t b = start_block + old_blocks; b < start_block + new_blocks; b++) {
            if (fs_bitmap_get(device_context, b))
                return false;
        }
        for (uint64_t b = start_block + old_blocks; b < start_block + new_blocks; b++) {
            fs_bitmap_set(device_context, b);
        }
        fs_bitmap_flush(device_context);
        if (device_context->alloc_hint < start_block + new_blocks &&
            device_context->alloc_hint >= start_block)
            device_context->alloc_hint = start_block + new_blocks;
    } else if (new_blocks < old_blocks) {
        for (uint64_t b = start_block + new_blocks; b < start_block + old_blocks; b++) {
            fs_bitmap_clear(device_context, b);
        }
        fs_bitmap_flush(device_context);
    }
    
    if (header.size != new_size) {
        header.size = new_size;
        fs_mem_write(device_context, alloc_addr, &header, sizeof(struct FSAllocHeader));
    }
    return true;
}

void fs_free(struct FSDeviceContext* device_context, uint64_t address) {
    if (!device_context || address == FS_NULL || address < sizeof(struct FSAllocHeader)) return;
    uint64_t alloc_addr = address - sizeof(struct FSAllocHeader);
    struct FSAllocHeader header;
    fs_mem_read(device_context, alloc_addr, &header, sizeof(struct FSAllocHeader));
    if (header.size > device_context->pool_size)
        return;     // Corrupt header: freeing its "blocks" would wreck the bitmap
    
    uint64_t blocks = fs_alloc_blocks_for(device_context, header.size);
    uint64_t start_block = alloc_addr / device_context->sector_size;
    
    for (uint64_t i = 0; i < blocks; i++) {
        fs_bitmap_clear(device_context, start_block + i);
    }
    
    header.size = 0;
    fs_mem_write(device_context, alloc_addr, &header, sizeof(struct FSAllocHeader));
    fs_bitmap_flush(device_context);
}

uint64_t fs_find_next(struct FSDeviceContext* device_context, uint64_t prev_addr) {
    if (!device_context) return FS_NULL;
    uint64_t start_block;
    struct FSAllocHeader header;
    
    if (prev_addr == FS_NULL) {
        start_block = device_context->reserved_blocks;
    } else {
        uint64_t h_addr = prev_addr - sizeof(struct FSAllocHeader);
        fs_mem_read(device_context, h_addr, &header, sizeof(struct FSAllocHeader));
        if (header.size > device_context->pool_size)
            return FS_NULL;     // Corrupt header: cannot tell where the next one starts
        uint64_t used = fs_alloc_blocks_for(device_context, header.size);
        start_block = (h_addr / device_context->sector_size) + used;
    }
    
    for (uint64_t i = start_block; i < device_context->block_count; i++) {
        if (fs_bitmap_get(device_context, i)) {
            uint64_t addr = i * device_context->sector_size;
            fs_mem_read(device_context, addr, &header, sizeof(struct FSAllocHeader));
            if (header.size != 0) return addr + sizeof(struct FSAllocHeader);
        }
    }
    return FS_NULL;
}

uint64_t fs_device_max_capacity(struct BlockDevice* device) {
    // pool_size is 64-bit: the whole device is addressable
    return block_capacity_bytes(device);
}

bool fs_device_format(struct BlockDevice* device, uint8_t* window_address, uint64_t capacity, uint32_t sector_size) {
    if (device == NULL || window_address == NULL)
        return false;
    if (!fs_block_size_valid(sector_size) || capacity < sector_size)
        return false;
    if (capacity > fs_device_max_capacity(device))
        return false;   // Would place blocks past the end of the disk
    
    struct FSDeviceHeader devH = { .id = FS_DEVICE_ID, .name = "fs" };
    struct FSPartitionBlock partH = {
        .total_size = capacity, 
        .sector_size = sector_size, 
        // No root yet: FS_NULL (not 0) until it is created below, so an
        // interrupted format never points the root at block 0
        .root_directory = FS_NULL,
        .version = FS_FORMAT_VERSION,
        .magic = FS_MAGIC, 
        .name = "storage"
    };
    struct FSPartitionBlock part;
    struct FSDeviceContext ctx = fs_device_open(device, window_address, &part);

    uint64_t b_size = ((capacity / sector_size) + 7) / 8;
    uint64_t meta_size = sizeof(devH) + sizeof(partH) + b_size;
    uint64_t res_blocks = (meta_size + sector_size - 1) / sector_size;
    
    fs_mem_write(&ctx, 0, &devH, sizeof(devH));
    fs_mem_write(&ctx, sizeof(devH), &partH, sizeof(partH));
    
    // The bitmap, written once and in bulk: the blocks under the metadata
    // (the leading bits) taken, everything else free. Nothing past the
    // bitmap is touched, which is what keeps a quick format quick.
    uint64_t b_addr = sizeof(devH) + sizeof(partH);
    uint64_t full_bytes = res_blocks / 8;
    if (full_bytes > b_size)
        full_bytes = b_size;
    fs_mem_fill(&ctx, b_addr, 0xFF, full_bytes);
    if (full_bytes < b_size) {
        uint8_t partial = (uint8_t)((1U << (res_blocks & 7U)) - 1U);
        fs_mem_write(&ctx, b_addr + full_bytes, &partial, 1);
        fs_mem_zero(&ctx, b_addr + full_bytes + 1, b_size - full_bytes - 1);
    }
    
    // The next open shares this sector buffer and starts with an invalid
    // sector_frame, so it would re-read sector 0 over the unwritten header.
    fs_cache_sync(&ctx);
    
    ctx = fs_device_open(device, window_address, &partH);
    if (!ctx.is_open)
        return false;   // Header did not read back: the writes failed
    
    // Root directory. Its parent is FS_NULL, which is also how the VFS
    // recognizes a device's root (see do_directory_check_mounted). It has
    // no entry anywhere, so it has no name either.
    uint64_t root = fs_directory_create(&ctx, NULL, FS_PERMISSION_READ | FS_PERMISSION_WRITE, FS_NULL);
    if (root == FS_NULL) {
        fs_cache_sync(&ctx);
        return false;   // Device too small to hold the metadata
    }
    
    partH.root_directory = root;
    fs_mem_write(&ctx, sizeof(devH), &partH, sizeof(partH));
    
    return fs_device_sync(&ctx);
}

void fs_device_format_low(struct BlockDevice* device, uint8_t* window_address, uint64_t capacity) {
    if (device == NULL || window_address == NULL) return;
    if (capacity > fs_device_max_capacity(device))
        capacity = fs_device_max_capacity(device);
    
    uint8_t clear_byte = 0x00;
    struct FSPartitionBlock part;
    struct FSDeviceContext ctx = fs_device_open(device, window_address, &part);
    
    for (uint64_t address_range = 0; address_range < capacity; address_range++) 
        fs_writeb(&ctx, address_range, clear_byte);
    fs_cache_sync(&ctx);
}

bool fs_device_sync(struct FSDeviceContext* ctx) {
    if (!ctx) return false;
    
    // Order matters: the bitmap frame is written into the sector cache,
    // which must then be written to the disk, which must then commit its
    // own cache. Doing it the other way round loses the bitmap.
    fs_bitmap_flush(ctx);
    
    bool ok = fs_cache_flush(ctx);
    return ok && !ctx->frame_dirty && !ctx->sector_dirty;
}

bool fs_check_directory_valid(struct FSDeviceContext* ctx, uint64_t address) {
    if (!ctx || address == FS_NULL || address < sizeof(struct FSAllocHeader)) return false;
    struct FSAllocHeader alloc;
    fs_mem_read(ctx, address - sizeof(struct FSAllocHeader), &alloc, sizeof(struct FSAllocHeader));
    if (alloc.size < sizeof(struct FSDirectoryHeader) || alloc.size > ctx->pool_size)
        return false;
    struct FSBlockHeader block;
    fs_mem_read(ctx, address, &block, sizeof(struct FSBlockHeader));
    return (block.attributes & FS_ATTRIBUTE_DIRECTORY) != 0;
}
