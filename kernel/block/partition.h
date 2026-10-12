#ifndef _KERNEL_BLOCK_PARTITION_H_
#define _KERNEL_BLOCK_PARTITION_H_

// Partitions
//
// A partition is a BlockDevice that is a window onto another one: sector 0
// of the partition is sector `start` of the disk, and it is `count` sectors
// long. Reads and writes are offset and bounds-checked here, so a filesystem
// mounted on a partition can never reach outside it. That is what keeps a
// format or wipe from touching the boot loader at the front of a boot disk.
//
// Partitions are found through a classic MBR partition table in sector 0.
// Nothing above this layer needs to know a device is a partition.
//
// No architecture dependencies: only stdint / stdbool / stddef.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <kernel/block/block.h>

#define MBR_PARTITION_COUNT     4
#define MBR_SECTOR_SIZE         512

// Partition type bytes
#define MBR_TYPE_EMPTY          0x00
#define MBR_TYPE_GPT_PROTECTIVE 0xEE
#define MBR_TYPE_BASEFS         0xDA    // Used by build-disk.sh; keep them in sync

struct MbrPartition {
    uint8_t  number;        // 1..4: the table slot, also the name suffix (ata0p1)
    uint8_t  type;          // MBR_TYPE_*
    bool     active;        // Boot flag (0x80)
    bool     usable;        // Inside the disk, does not start at sector 0,
                            // does not overlap another entry
    uint64_t start;         // First sector on the disk
    uint64_t count;         // Length in sectors
};

// Parse an MBR from a disk's first sector (MBR_SECTOR_SIZE bytes).
//
// Returns false when the sector does not hold a partition table: no 55 AA
// signature, or status bytes other than 0x00 / 0x80 (a boot sector or plain
// data that happens to end in 55 AA).
//
// Returns true otherwise and fills `out` with the non-empty entries, in
// slot order, setting *count to how many (0 for an empty table). Entries
// that are out of range or overlap are reported with usable = false.
bool block_mbr_parse(const uint8_t* sector0, uint64_t disk_sectors,
                     struct MbrPartition out[MBR_PARTITION_COUNT], uint8_t* count);

// Write a fresh MBR to `disk` with one active partition of `type` covering
// [start, end of disk). Everything else in sector 0 is preserved (boot code
// and disk signature), so a disk built by build-disk.sh keeps booting.
// Any partition devices already created for this disk are stale afterwards;
// the new table is picked up at the next scan (reboot).
bool block_mbr_write_single(struct BlockDevice* disk, uint8_t type, uint64_t start);

// Make `disk` boot like the disk the image came from. `image` is a boot
// area as stage 2 hands it to the kernel: sector 0 (MBR with stage 1),
// stage 2 and the kernel file, `image_sectors` long. Sectors 1.. are written
// as they are; sector 0 keeps the image's boot code but gets a new table
// with one active partition of `type` from `data_start` to the end of the
// disk. Everything on the disk is lost; anything mounted from it must be
// released first (vfs_device_release_disk).
bool block_install_boot_area(struct BlockDevice* disk, const uint8_t* image, uint32_t image_sectors,
                             uint8_t type, uint64_t data_start);

// Register a partition of `parent` as its own BlockDevice, named
// "<parent>p<number>" ("ata0p1", "ahci0p2"). Calling it again for the same
// range returns the existing device. Returns NULL if the range does not fit
// the parent, starts at sector 0, or the parent is itself a partition.
struct BlockDevice* block_partition_create(struct BlockDevice* parent, uint8_t number,
                                           uint64_t start, uint64_t count);

// The disk a partition lives on, or NULL if `dev` is not a partition
struct BlockDevice* block_partition_parent(const struct BlockDevice* dev);

// First sector of a partition on its disk (0 for a whole disk)
uint64_t block_partition_start(const struct BlockDevice* dev);

//
// Disk scan
//
// Decide what on a freshly registered disk should be mounted:
//
//   1. Sector 0 holds a whole-disk volume (probe() says so): the disk itself.
//   2. Sector 0 holds an MBR: one partition device per usable entry of
//      `type`, never the disk itself, so nothing mounted can overwrite the
//      table or the boot code and kernel in front of the first partition.
//   3. Anything else (blank, unformatted, unreadable): the disk itself, so
//      it can be formatted as a whole-disk volume, exactly as before.
//
// mount() is called once per device to mount, in that order.

typedef enum {
    BLOCK_SCAN_WHOLE_VOLUME,    // Case 1: disk mounted whole, it holds a volume
    BLOCK_SCAN_WHOLE_BLANK,     // Case 3: disk mounted whole, nothing recognized
    BLOCK_SCAN_PARTITIONS,      // Case 2: at least one partition mounted
    BLOCK_SCAN_NO_PARTITIONS,   // Case 2, but no usable entry of `type`: nothing mounted
    BLOCK_SCAN_ERROR            // Bad arguments, or partition devices could not be registered
} BlockScanResult;

struct BlockScanReport {
    BlockScanResult result;
    uint8_t mounted;            // Devices handed to mount()
    uint8_t skipped;            // Entries of `type` that were not usable
    uint8_t foreign;            // Entries of other types (left alone)
};

// True if this first sector belongs to a volume that spans the whole disk
typedef bool (*block_probe_fn)(const uint8_t* sector0, uint32_t sector_size);
typedef void (*block_mount_fn)(struct BlockDevice* device, void* arg);

// Not reentrant: uses one static sector buffer. Call from the boot-time scan.
struct BlockScanReport block_scan_disk(struct BlockDevice* disk, uint8_t type,
                                       block_probe_fn probe, block_mount_fn mount, void* arg);

#endif
