
#ifndef _KERNEL_DRIVERS_ATA_H_
#define _KERNEL_DRIVERS_ATA_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define ATA_SECTOR_SIZE 512

// Two channels (primary / secondary), two drives each (master / slave)
#define ATA_MAX_DEVICES 4

#define ATA_PRIMARY_IO      0x1F0
#define ATA_PRIMARY_CTRL    0x3F6
#define ATA_SECONDARY_IO    0x170
#define ATA_SECONDARY_CTRL  0x376

struct AtaChannel;

// One detected ATA hard disk. ATAPI devices (CD-ROMs) are skipped.
struct AtaDevice {
    struct AtaChannel* channel;
    uint16_t io_base;
    uint16_t ctrl_base;
    uint8_t  slave;          // 0 = master, 1 = slave
    uint8_t  index;          // Registration order (ata0, ata1, ...)
    uint32_t sector_count;   // LBA28 addressable sectors (IDENTIFY words 60-61)
};

//
// Detection
//

// Probe both drives on one channel and register every ATA disk found.
// Returns how many disks were registered. Safe to call once per channel.
uint8_t ata_probe_channel(uint16_t io_base, uint16_t ctrl_base);

uint8_t ata_device_count(void);
struct AtaDevice* ata_get_device(uint8_t index);

//
// Per-device access (preferred). Every call is serialized per channel,
// since master and slave share the same task file registers.
//

bool ata_dev_read_sector(struct AtaDevice* dev, uint32_t address, uint8_t* buffer);
bool ata_dev_write_sector(struct AtaDevice* dev, uint32_t address, const uint8_t* buffer);
bool ata_dev_flush(struct AtaDevice* dev);

bool ata_dev_read_bytes(struct AtaDevice* dev, uint32_t address, uint8_t* buffer, size_t total_bytes);
bool ata_dev_write_bytes(struct AtaDevice* dev, uint32_t address, const uint8_t* buffer, size_t total_bytes);

//
// Block layer
//
// Register a detected disk as a generic block device ("ata0", "ata1", ...).
// This is the only way the rest of the kernel should reach a disk; there is
// deliberately no "current drive" API any more, because one is how a format
// of ata1 ended up writing to ata0. Returns the existing device if this disk
// is already registered, NULL if registration failed.

struct BlockDevice;
struct BlockDevice* ata_create_block_device(struct AtaDevice* dev);

#endif
