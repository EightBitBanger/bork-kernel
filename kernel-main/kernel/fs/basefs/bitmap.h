#ifndef BASE_BITMAP_H
#define BASE_BITMAP_H

#include <kernel/fs/config.h>
#include <kernel/fs/basefs/structs.h>

bool fs_bitmap_get(struct FSDeviceContext* ctx, uint64_t index);
void fs_bitmap_set(struct FSDeviceContext* ctx, uint64_t index);
uint8_t fs_bitmap_get_byte(struct FSDeviceContext* ctx, uint64_t byte_index);
void fs_bitmap_clear(struct FSDeviceContext* ctx, uint64_t index);

void fs_bitmap_sync(struct FSDeviceContext* ctx, uint64_t bit_index);
void fs_bitmap_flush(struct FSDeviceContext* ctx);

#endif
