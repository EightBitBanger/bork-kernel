#ifndef _KERNEL_BLOCK_DEVICE_H_
#define _KERNEL_BLOCK_DEVICE_H_

// Block devices
//
// A storage driver (ATA, AHCI, RAM disk..) registers each disk it finds 
// as a BlockDevice with a small table of operations. Everything above 
// the drivers, the filesystem included, only ever sees BlockDevice*
// and addresses sectors through block_read / block_write / block_flush.
// Nothing above this layer knows which controller or port a disk is on.
//
// No architecture dependencies: only stdint / stdbool / stddef.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define BLOCK_DEVICE_MAX    32      // Disks and the partitions on them
#define BLOCK_NAME_MAX      16      // Including the terminator ("ahci12" etc.)

struct BlockDevice;

struct BlockDeviceOps {
    // Transfer `count` whole sectors starting at `lba`. The block layer has
    // already range-checked the request against sector_count.
    bool (*read)(struct BlockDevice* dev, uint64_t lba, uint32_t count, void* buffer);
    bool (*write)(struct BlockDevice* dev, uint64_t lba, uint32_t count, const void* buffer);

    // Commit the device's volatile write cache. NULL if there is none.
    bool (*flush)(struct BlockDevice* dev);
};

struct BlockDevice {
    char     name[BLOCK_NAME_MAX];  // "ata0", "ahci1", ... (also the /mnt name)
    uint32_t sector_size;           // Bytes per sector (512 for ATA/AHCI)
    uint64_t sector_count;          // Addressable sectors

    const struct BlockDeviceOps* ops;
    void*    driver_data;           // Driver's own handle (AtaDevice*, AHCI port...)
};

// Register a device named prefix + the next free number ("ata" -> "ata0",
// "ata1", ...). Returns the existing device if driver_data is already
// registered.
struct BlockDevice* block_register(const char* prefix, const struct BlockDeviceOps* ops,
                                   void* driver_data, uint32_t sector_size, uint64_t sector_count);

// Same, but with an exact name ("ata0p1"). Fails if another device already
// has that name.
struct BlockDevice* block_register_named(const char* name, const struct BlockDeviceOps* ops,
                                         void* driver_data, uint32_t sector_size, uint64_t sector_count);

uint32_t            block_device_count(void);
struct BlockDevice* block_get(uint32_t index);
struct BlockDevice* block_find(const char* name);
struct BlockDevice* block_find_driver(const void* driver_data);

// Bounds-checked transfers. A request that runs past the end of the device
// fails without touching it.
bool block_read(struct BlockDevice* dev, uint64_t lba, uint32_t count, void* buffer);
bool block_write(struct BlockDevice* dev, uint64_t lba, uint32_t count, const void* buffer);
bool block_flush(struct BlockDevice* dev);

uint64_t block_capacity_bytes(const struct BlockDevice* dev);

#endif
