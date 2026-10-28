#include <kernel/block/partition.h>
#include <kernel/util/timer.h>

// Partition devices and MBR scanning. See partition.h.
//
// Like block.c, everything here runs during the boot-time bus scan on one
// thread, except the partition read/write/flush operations, which are
// serialized by whoever owns the mounted device (the VFS lock) and by the
// disk's own driver.

#define MBR_TABLE_OFFSET    0x1BE
#define MBR_ENTRY_SIZE      16
#define MBR_STATUS_ACTIVE   0x80

// One sector of the disk being scanned or rewritten. Static, so it is mapped
// for DMA drivers (AHCI translates it page by page) and never on a stack.
// Large enough for the biggest sector size the scan will read.
#define SCAN_BUFFER_SIZE    4096
static uint8_t scan_buffer[SCAN_BUFFER_SIZE] __attribute__((aligned(16)));

//
// Partition devices
//

struct BlockPartition {
    struct BlockDevice* parent;
    uint64_t start;
    uint64_t count;
    uint8_t  number;    // Table slot, also the name suffix
    bool     used;
};

static struct BlockPartition partitions[BLOCK_DEVICE_MAX];

static bool partition_in_range(const struct BlockPartition* p, uint64_t lba, uint32_t count) {
    // The block layer already checked this against sector_count (== p->count).
    // Checked again here because this is the one place that guarantees a
    // partition never reaches the sectors in front of it.
    return count != 0 && lba < p->count && (uint64_t)count <= p->count - lba;
}

static bool partition_read(struct BlockDevice* dev, uint64_t lba, uint32_t count, void* buffer) {
    struct BlockPartition* p = (struct BlockPartition*)dev->driver_data;
    if (!partition_in_range(p, lba, count)) return false;
    return block_read(p->parent, p->start + lba, count, buffer);
}

static bool partition_write(struct BlockDevice* dev, uint64_t lba, uint32_t count, const void* buffer) {
    struct BlockPartition* p = (struct BlockPartition*)dev->driver_data;
    if (!partition_in_range(p, lba, count)) return false;
    return block_write(p->parent, p->start + lba, count, buffer);
}

static bool partition_flush(struct BlockDevice* dev) {
    struct BlockPartition* p = (struct BlockPartition*)dev->driver_data;
    return block_flush(p->parent);
}

static const struct BlockDeviceOps partition_ops = {
    .read  = partition_read,
    .write = partition_write,
    .flush = partition_flush,
};

struct BlockDevice* block_partition_parent(const struct BlockDevice* dev) {
    if (dev == NULL || dev->ops != &partition_ops) return NULL;
    return ((const struct BlockPartition*)dev->driver_data)->parent;
}

uint64_t block_partition_start(const struct BlockDevice* dev) {
    if (dev == NULL || dev->ops != &partition_ops) return 0;
    return ((const struct BlockPartition*)dev->driver_data)->start;
}

// "<parent>p<number>". False if it does not fit BLOCK_NAME_MAX.
static bool partition_make_name(char* out, const char* parent, uint8_t number) {
    size_t len = 0;
    while (parent[len] != '\0') len++;

    char digits[3];
    size_t ndigits = 0;
    do {
        digits[ndigits++] = (char)('0' + (number % 10));
        number /= 10;
    } while (number != 0);

    if (len + 1 + ndigits + 1 > BLOCK_NAME_MAX)
        return false;

    for (size_t i = 0; i < len; i++) out[i] = parent[i];
    out[len] = 'p';
    for (size_t i = 0; i < ndigits; i++) out[len + 1 + i] = digits[ndigits - 1 - i];
    out[len + 1 + ndigits] = '\0';
    return true;
}

struct BlockDevice* block_partition_create(struct BlockDevice* parent, uint8_t number,
                                           uint64_t start, uint64_t count) {
    if (parent == NULL || number == 0)
        return NULL;
    if (parent->ops == &partition_ops)
        return NULL;    // No partitions inside partitions
    if (start == 0 || count == 0 || start >= parent->sector_count || count > parent->sector_count - start)
        return NULL;    // Sector 0 holds the table; the rest must fit the disk

    // Same slot again (a second scan, or the table was rewritten): hand back
    // the existing device, moved to the range the table has now
    struct BlockPartition* slot = NULL;
    for (uint32_t i = 0; i < BLOCK_DEVICE_MAX; i++) {
        struct BlockPartition* p = &partitions[i];
        if (p->used && p->parent == parent && p->number == number) {
            struct BlockDevice* dev = block_find_driver(p);
            if (dev == NULL)
                return NULL;
            p->start          = start;
            p->count          = count;
            dev->sector_count = count;
            return dev;
        }
        if (!p->used && slot == NULL)
            slot = p;
    }
    if (slot == NULL)
        return NULL;

    char name[BLOCK_NAME_MAX];
    if (!partition_make_name(name, parent->name, number))
        return NULL;

    slot->parent = parent;
    slot->start  = start;
    slot->count  = count;
    slot->number = number;
    slot->used   = true;

    struct BlockDevice* dev = block_register_named(name, &partition_ops, slot,
                                                   parent->sector_size, count);
    if (dev == NULL)
        slot->used = false;
    return dev;
}

void block_partition_drop_all(struct BlockDevice* disk) {
    for (uint32_t i = 0; i < BLOCK_DEVICE_MAX; i++) {
        struct BlockPartition* p = &partitions[i];
        if (!p->used || p->parent != disk)
            continue;
        struct BlockDevice* dev = block_find_driver(p);
        p->count = 0;
        if (dev != NULL)
            dev->sector_count = 0;
    }
}

void block_partition_remove_all(struct BlockDevice* disk) {
    for (uint32_t i = 0; i < BLOCK_DEVICE_MAX; i++) {
        struct BlockPartition* p = &partitions[i];
        if (!p->used || p->parent != disk)
            continue;
        struct BlockDevice* dev = block_find_driver(p);
        if (dev != NULL)
            block_unregister(dev);
        p->used = false;
        p->parent = NULL;
    }
}

//
// MBR
//

static inline uint32_t mbr_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void mbr_put_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

bool block_mbr_parse(const uint8_t* sector0, uint64_t disk_sectors,
                     struct MbrPartition out[MBR_PARTITION_COUNT], uint8_t* count) {
    if (count != NULL) *count = 0;
    if (sector0 == NULL || out == NULL || count == NULL)
        return false;
    if (sector0[510] != 0x55 || sector0[511] != 0xAA)
        return false;

    // A real table only ever has 0x00 or 0x80 here. Boot sectors (FAT and
    // the like) that merely end in 55 AA almost never do.
    for (int i = 0; i < MBR_PARTITION_COUNT; i++) {
        uint8_t status = sector0[MBR_TABLE_OFFSET + i * MBR_ENTRY_SIZE];
        if (status != 0x00 && status != MBR_STATUS_ACTIVE)
            return false;
    }

    uint8_t n = 0;
    for (int i = 0; i < MBR_PARTITION_COUNT; i++) {
        const uint8_t* e = &sector0[MBR_TABLE_OFFSET + i * MBR_ENTRY_SIZE];
        uint8_t type = e[4];
        if (type == MBR_TYPE_EMPTY)
            continue;

        struct MbrPartition* p = &out[n++];
        p->number = (uint8_t)(i + 1);
        p->type   = type;
        p->active = (e[0] == MBR_STATUS_ACTIVE);
        p->start  = mbr_le32(e + 8);
        p->count  = mbr_le32(e + 12);
        p->usable = p->start != 0 && p->count != 0 &&
                    p->start < disk_sectors && p->count <= disk_sectors - p->start;
    }

    // Overlapping entries: neither can be trusted
    for (uint8_t a = 0; a < n; a++) {
        for (uint8_t b = (uint8_t)(a + 1); b < n; b++) {
            if (out[a].count == 0 || out[b].count == 0)
                continue;
            uint64_t a_end = out[a].start + out[a].count;
            uint64_t b_end = out[b].start + out[b].count;
            if (out[a].start < b_end && out[b].start < a_end) {
                out[a].usable = false;
                out[b].usable = false;
            }
        }
    }

    *count = n;
    return true;
}

// Clear the table in an MBR sector and describe one active partition
uint32_t block_mbr_signature(const uint8_t* sector0) {
    if (sector0 == NULL || sector0[510] != 0x55 || sector0[511] != 0xAA)
        return 0;
    return mbr_le32(sector0 + MBR_SIGNATURE_OFFSET);
}

// A fresh disk signature for `disk`: non-zero and not `avoid`. It only has
// to tell this machine's disks apart, so FNV-1a over the time since boot,
// the disk's name and size and what its sector 0 held before (`old`, one
// MBR_SECTOR_SIZE sector) is enough.
static uint32_t mbr_new_signature(const struct BlockDevice* disk, const uint8_t* old, uint32_t avoid) {
    uint32_t hash = 2166136261u;
    uint64_t mix[2] = { timer_get_ms(), disk->sector_count };

    const uint8_t* bytes = (const uint8_t*)mix;
    for (uint32_t i = 0; i < sizeof(mix); i++)
        hash = (hash ^ bytes[i]) * 16777619u;
    for (uint32_t i = 0; i < BLOCK_NAME_MAX && disk->name[i] != '\0'; i++)
        hash = (hash ^ (uint8_t)disk->name[i]) * 16777619u;
    for (uint32_t i = 0; i < MBR_SECTOR_SIZE; i++)
        hash = (hash ^ old[i]) * 16777619u;

    while (hash == 0 || hash == avoid)
        hash = hash * 16777619u + 1;
    return hash;
}

static void mbr_fill_single(uint8_t* sector, uint8_t type, uint64_t start, uint64_t count) {
    for (uint32_t i = MBR_TABLE_OFFSET; i < 510; i++)
        sector[i] = 0;

    uint8_t* e = &sector[MBR_TABLE_OFFSET];
    e[0] = MBR_STATUS_ACTIVE;
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;      // CHS start: "use LBA"
    e[4] = type;
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;      // CHS end: "use LBA"
    mbr_put_le32(e + 8,  (uint32_t)start);
    mbr_put_le32(e + 12, (uint32_t)count);

    sector[510] = 0x55;
    sector[511] = 0xAA;
}

// Sectors from `start` to the end of the disk, capped at what an MBR entry holds
static uint64_t mbr_count_to_end(const struct BlockDevice* disk, uint64_t start) {
    uint64_t count = disk->sector_count - start;
    return (count > 0xFFFFFFFFULL) ? 0xFFFFFFFFULL : count;
}

static bool mbr_disk_ok(const struct BlockDevice* disk, uint8_t type, uint64_t start) {
    return disk != NULL && type != MBR_TYPE_EMPTY && block_partition_parent(disk) == NULL &&
           disk->sector_size == MBR_SECTOR_SIZE &&
           start != 0 && start < disk->sector_count && start <= 0xFFFFFFFFULL;   // MBR fields are 32-bit
}

bool block_mbr_write_single(struct BlockDevice* disk, uint8_t type, uint64_t start) {
    if (!mbr_disk_ok(disk, type, start))
        return false;

    // Keep the boot code and disk signature if sector 0 already holds an MBR
    // (a disk from build-disk.sh); otherwise start from a clean sector.
    struct MbrPartition table[MBR_PARTITION_COUNT];
    uint8_t entries;
    bool keep = block_read(disk, 0, 1, scan_buffer) &&
                block_mbr_parse(scan_buffer, disk->sector_count, table, &entries);
    uint32_t signature = keep ? block_mbr_signature(scan_buffer) : 0;
    if (signature == 0)
        signature = mbr_new_signature(disk, scan_buffer, 0);
    if (!keep) {
        for (uint32_t i = 0; i < MBR_SECTOR_SIZE; i++)
            scan_buffer[i] = 0;
    }

    mbr_put_le32(scan_buffer + MBR_SIGNATURE_OFFSET, signature);
    mbr_fill_single(scan_buffer, type, start, mbr_count_to_end(disk, start));
    return block_write(disk, 0, 1, scan_buffer) && block_flush(disk);
}

bool block_mbr_write(struct BlockDevice* disk, const struct MbrPartition* entries, uint8_t count,
                     bool keep_code) {
    if (disk == NULL || block_partition_parent(disk) != NULL || disk->sector_size != MBR_SECTOR_SIZE)
        return false;
    if (count > MBR_PARTITION_COUNT || (count > 0 && entries == NULL))
        return false;

    uint8_t active = 0;
    for (uint8_t a = 0; a < count; a++) {
        const struct MbrPartition* e = &entries[a];
        if (e->number < 1 || e->number > MBR_PARTITION_COUNT || e->type == MBR_TYPE_EMPTY)
            return false;
        if (e->start == 0 || e->count == 0 || e->start >= disk->sector_count ||
            e->count > disk->sector_count - e->start ||
            e->start > 0xFFFFFFFFULL || e->count > 0xFFFFFFFFULL)
            return false;
        if (e->active)
            active++;
        for (uint8_t b = 0; b < a; b++) {
            const struct MbrPartition* f = &entries[b];
            if (f->number == e->number)
                return false;
            if (e->start < f->start + f->count && f->start < e->start + e->count)
                return false;
        }
    }
    if (active > 1)
        return false;

    struct MbrPartition old[MBR_PARTITION_COUNT];
    uint8_t old_count;
    if (!block_read(disk, 0, 1, scan_buffer))
        return false;
    bool is_mbr = block_mbr_parse(scan_buffer, disk->sector_count, old, &old_count);
    uint32_t signature = is_mbr ? block_mbr_signature(scan_buffer) : 0;
    if (signature == 0)
        signature = mbr_new_signature(disk, scan_buffer, 0);
    if (!is_mbr || !keep_code) {
        for (uint32_t i = 0; i < MBR_SECTOR_SIZE; i++)
            scan_buffer[i] = 0;
    }

    for (uint32_t i = MBR_TABLE_OFFSET; i < 510; i++)
        scan_buffer[i] = 0;
    mbr_put_le32(scan_buffer + MBR_SIGNATURE_OFFSET, signature);
    for (uint8_t a = 0; a < count; a++) {
        const struct MbrPartition* p = &entries[a];
        uint8_t* e = &scan_buffer[MBR_TABLE_OFFSET + (p->number - 1) * MBR_ENTRY_SIZE];
        e[0] = p->active ? MBR_STATUS_ACTIVE : 0x00;
        e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;      // CHS start: "use LBA"
        e[4] = p->type;
        e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;      // CHS end: "use LBA"
        mbr_put_le32(e + 8,  (uint32_t)p->start);
        mbr_put_le32(e + 12, (uint32_t)p->count);
    }
    scan_buffer[510] = 0x55;
    scan_buffer[511] = 0xAA;

    return block_write(disk, 0, 1, scan_buffer) && block_flush(disk);
}

bool block_mbr_renew_signature(struct BlockDevice* disk, uint32_t avoid) {
    if (disk == NULL || disk->sector_size != MBR_SECTOR_SIZE || !block_read(disk, 0, 1, scan_buffer))
        return false;
    if (scan_buffer[510] != 0x55 || scan_buffer[511] != 0xAA)
        return false;

    uint32_t signature = block_mbr_signature(scan_buffer);
    if (signature != 0 && signature != avoid)
        return true;

    mbr_put_le32(scan_buffer + MBR_SIGNATURE_OFFSET, mbr_new_signature(disk, scan_buffer, avoid));
    return block_write(disk, 0, 1, scan_buffer) && block_flush(disk);
}

#define INSTALL_CHUNK_SECTORS  64

bool block_install_boot_area(struct BlockDevice* disk, const uint8_t* image, uint32_t image_sectors,
                             uint8_t type, uint64_t data_start) {
    if (!mbr_disk_ok(disk, type, data_start) || image == NULL)
        return false;
    if (image_sectors < 2 || image_sectors > data_start)
        return false;   // Nothing to install, or it would run into the partition
    if (image[510] != 0x55 || image[511] != 0xAA)
        return false;   // Sector 0 of the image is not a boot sector

    // The disk's own signature, or a fresh one when it has none or shares
    // the image's (a clone of the boot disk). Read before anything is
    // written; the old sector 0 also seeds the fresh signature.
    uint32_t image_signature = block_mbr_signature(image);
    if (!block_read(disk, 0, 1, scan_buffer))
        return false;
    uint32_t signature = block_mbr_signature(scan_buffer);
    if (signature == 0 || signature == image_signature)
        signature = mbr_new_signature(disk, scan_buffer, image_signature);

    // Stage 2 and the kernel first, sector 0 last: until the new MBR lands,
    // the disk keeps whatever it had in sector 0
    for (uint32_t lba = 1; lba < image_sectors; ) {
        uint32_t count = image_sectors - lba;
        if (count > INSTALL_CHUNK_SECTORS)
            count = INSTALL_CHUNK_SECTORS;
        if (!block_write(disk, lba, count, image + (uint64_t)lba * MBR_SECTOR_SIZE))
            return false;
        lba += count;
    }

    // The image's boot code with a table describing THIS disk
    for (uint32_t i = 0; i < MBR_SECTOR_SIZE; i++)
        scan_buffer[i] = image[i];
    mbr_put_le32(scan_buffer + MBR_SIGNATURE_OFFSET, signature);
    mbr_fill_single(scan_buffer, type, data_start, mbr_count_to_end(disk, data_start));

    return block_write(disk, 0, 1, scan_buffer) && block_flush(disk);
}

//
// Disk scan
//

struct BlockScanReport block_scan_disk(struct BlockDevice* disk, block_type_fn accepts,
                                       block_probe_fn probe, block_mount_fn mount, void* arg) {
    struct BlockScanReport report = { BLOCK_SCAN_ERROR, 0, 0, 0 };
    if (disk == NULL || mount == NULL || accepts == NULL)
        return report;

    bool readable = disk->sector_size <= SCAN_BUFFER_SIZE &&
                    block_read(disk, 0, 1, scan_buffer);

    // 1. A volume that spans the whole disk. Checked before the MBR: a
    //    whole-disk volume's own metadata may happen to end in 55 AA.
    if (readable && probe != NULL && probe(scan_buffer, disk->sector_size)) {
        mount(disk, arg);
        report.result  = BLOCK_SCAN_WHOLE_VOLUME;
        report.mounted = 1;
        return report;
    }

    // 2. A partition table. Partitions are never scanned for tables of
    //    their own.
    struct MbrPartition table[MBR_PARTITION_COUNT];
    uint8_t entries = 0;
    if (readable && disk->sector_size == MBR_SECTOR_SIZE &&
        block_partition_parent(disk) == NULL &&
        block_mbr_parse(scan_buffer, disk->sector_count, table, &entries)) {

        bool registration_failed = false;
        for (uint8_t i = 0; i < entries; i++) {
            if (!accepts(table[i].type)) {
                report.foreign++;
                continue;
            }
            if (!table[i].usable) {
                report.skipped++;
                continue;
            }
            struct BlockDevice* part = block_partition_create(disk, table[i].number,
                                                              table[i].start, table[i].count);
            if (part == NULL) {
                report.skipped++;
                registration_failed = true;
                continue;
            }
            mount(part, arg);
            report.mounted++;
        }

        if (report.mounted > 0)
            report.result = BLOCK_SCAN_PARTITIONS;
        else if (registration_failed)
            report.result = BLOCK_SCAN_ERROR;
        else if (entries > 0)
            report.result = BLOCK_SCAN_NO_PARTITIONS;
        if (entries > 0 || registration_failed)
            return report;
        // An empty table (every partition deleted): fall through and mount
        // the blank disk, so it still shows in /mnt
    }

    // 3. Blank, unformatted, unreadable or an empty table: the whole disk,
    //    so it can be formatted or partitioned
    mount(disk, arg);
    report.result  = BLOCK_SCAN_WHOLE_BLANK;
    report.mounted = 1;
    return report;
}
