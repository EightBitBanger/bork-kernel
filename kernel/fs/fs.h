#ifndef BASE_FILE_SYSTEM_H
#define BASE_FILE_SYSTEM_H

#include <kernel/util/delay.h>

#include <kernel/fs/basefs/io.h>
#include <kernel/fs/basefs/structs.h>
#include <kernel/fs/basefs/bitmap.h>
#include <kernel/fs/basefs/flags.h>

#include <kernel/fs/basefs/file.h>
#include <kernel/fs/basefs/directory.h>

#include <stdint.h>
#include <stdbool.h>

#define FS_MAGIC                0xAAUL
#define FS_NULL                 0xFFFFFFFFFFFFFFFFULL
#define FS_INVALID_FRAME        0xFFFFFFFFFFFFFFFFULL
#define FS_DEVICE_ID            0x13

void fs_init(void);

// Devices
//
// A filesystem lives on a generic BlockDevice (block/block.h) and caches one
// device sector in a caller-provided window of fs_device_window_size(device)
// bytes. Give every mounted context its OWN window: two contexts sharing one
// would serve each other's cached sectors.

// Format a device: header, partition block, allocation bitmap and an empty
// root directory. capacity must not exceed fs_device_max_capacity(device),
// and sector_size (the allocation block size) must be a multiple of 4 and
// at least FS_MIN_SECTOR_SIZE. Returns false if the device could not be
// formatted. Contexts already open
// on this device are stale afterwards; re-open them (see vfs_device_format).
bool fs_device_format(struct BlockDevice* device, uint8_t* window_address, uint64_t capacity, uint32_t sector_size);
void fs_device_format_low(struct BlockDevice* device, uint8_t* window_address, uint64_t capacity);

// Largest capacity basefs can address on this device (the whole device)
uint64_t fs_device_max_capacity(struct BlockDevice* device);

// Open a device. On an unformatted device, or one formatted with another
// format version, the context comes back with is_open == false, but still
// bound to the device and window.
struct FSDeviceContext fs_device_open(struct BlockDevice* device, uint8_t* window_address, struct FSPartitionBlock* partition);
uint64_t fs_get_used_bytes(struct FSDeviceContext* device_context);
uint8_t fs_device_get_partition(struct FSDeviceContext* device_context, struct FSPartitionBlock* part);

// True if `sector0` (a device's first sector, `size` bytes) starts a basefs
// volume of this format version. Used by the disk scan to tell a whole-disk
// volume from a partition table.
bool fs_device_probe(const uint8_t* sector0, uint32_t size);

uint64_t fs_find_next(struct FSDeviceContext* device_context, uint64_t previous_address);
void fs_bitmap_flush(struct FSDeviceContext* ctx);
uint32_t fs_bitmap_get_size(void);

uint64_t fs_alloc(struct FSDeviceContext* device_context, uint64_t size);
void fs_free(struct FSDeviceContext* device_context, uint64_t address);

// Change the size of an existing allocation in place. Shrinking releases
// whole blocks past the new end. Growing succeeds only if the blocks right
// after the allocation are free; otherwise nothing changes and it returns
// false. The payload address never moves.
bool fs_alloc_resize(struct FSDeviceContext* device_context, uint64_t address, uint64_t new_size);

// True if `address` is a live allocation holding a directory header
bool fs_check_directory_valid(struct FSDeviceContext* ctx, uint64_t address);

// Clock for the created/modified timestamps (seconds since the Unix epoch).
// basefs has no clock of its own: until a source is set, timestamps are 0.
void fs_set_time_source(uint32_t (*now)(void));
uint32_t fs_time_now(void);

void fs_cache_sync(struct FSDeviceContext* ctx);

// Push every pending change for a device to stable storage: the cached
// bitmap frame, the cached sector, and the drive's own write cache.
// Call before power-off. Returns false if any step failed.
bool fs_device_sync(struct FSDeviceContext* ctx);

#endif
