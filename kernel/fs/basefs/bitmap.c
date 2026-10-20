#include <kernel/fs/fs.h>

static inline uint64_t fs_bitmap_address(void) {
    return sizeof(struct FSDeviceHeader) + sizeof(struct FSPartitionBlock);
}

bool fs_bitmap_get(struct FSDeviceContext* ctx, uint64_t index) {
    if (!ctx) return false;
    fs_bitmap_sync(ctx, index);
    uint32_t local_byte = (uint32_t)((index >> 3) - ctx->frame_offset);
    return (ctx->frame_bitmap[local_byte] & (uint8_t)(1U << (index & 7U))) != 0;
}

void fs_bitmap_set(struct FSDeviceContext* ctx, uint64_t index) {
    if (!ctx) return;
    fs_bitmap_sync(ctx, index);
    uint32_t local_byte = (uint32_t)((index >> 3) - ctx->frame_offset);
    uint8_t mask = (uint8_t)(1U << (index & 7U));
    if (!(ctx->frame_bitmap[local_byte] & mask)) {
        ctx->frame_bitmap[local_byte] |= mask;
        ctx->frame_dirty = true;
    }
}

void fs_bitmap_clear(struct FSDeviceContext* ctx, uint64_t index) {
    if (!ctx) return;
    fs_bitmap_sync(ctx, index);
    uint32_t local_byte = (uint32_t)((index >> 3) - ctx->frame_offset);
    uint8_t mask = (uint8_t)(1U << (index & 7U));
    if (ctx->frame_bitmap[local_byte] & mask) {
        ctx->frame_bitmap[local_byte] &= ~mask;
        ctx->frame_dirty = true;
    }
}

void fs_bitmap_flush(struct FSDeviceContext* ctx) {
    if (!ctx) return;
    if (ctx->frame_dirty && ctx->frame_offset != FS_INVALID_FRAME) {
        uint32_t write_size = BITMAP_FRAME_SIZE;
        if (ctx->frame_offset + write_size > ctx->bitmap_size) {
            write_size = (uint32_t)(ctx->bitmap_size - ctx->frame_offset);
        }
        fs_mem_write(ctx, fs_bitmap_address() + ctx->frame_offset, ctx->frame_bitmap, write_size);
        ctx->frame_dirty = false;
    }
}

void fs_bitmap_sync(struct FSDeviceContext* ctx, uint64_t bit_index) {
    if (!ctx) return;
    uint64_t byte_index = bit_index >> 3;
    uint64_t window_start = (byte_index / BITMAP_FRAME_SIZE) * BITMAP_FRAME_SIZE;
    
    if (ctx->frame_offset != window_start) {
        fs_bitmap_flush(ctx);
        
        ctx->frame_offset = window_start;
        uint32_t read_size = BITMAP_FRAME_SIZE;
        if (ctx->frame_offset + read_size > ctx->bitmap_size) {
            read_size = (uint32_t)(ctx->bitmap_size - ctx->frame_offset);
        }
        
        fs_mem_read(ctx, fs_bitmap_address() + ctx->frame_offset, ctx->frame_bitmap, read_size);
        ctx->frame_dirty = false;
    }
}

uint8_t fs_bitmap_get_byte(struct FSDeviceContext* ctx, uint64_t byte_index) {
    if (!ctx) return 0xFF;
    fs_bitmap_sync(ctx, byte_index << 3);
    return ctx->frame_bitmap[(uint32_t)(byte_index - ctx->frame_offset)];
}
