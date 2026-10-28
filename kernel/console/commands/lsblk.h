#ifndef CONSOLE_CMD_LSBLK_H
#define CONSOLE_CMD_LSBLK_H

#include <stdint.h>
#include <stdbool.h>

#include <kernel/console/print.h>
#include <kernel/util/string.h>
#include <kernel/vfs/vfs.h>
#include <kernel/block/block.h>
#include <kernel/block/partition.h>

// lsblk [-b] [disk...]   list disks and the partitions on them
//
//   NAME          SIZE TYPE FSTYPE  MOUNTPOINT
//   ata0           64M disk
//   `-ata0p1       56M part basefs  /mnt/ata0p1
//   ata1          128M disk
//   |-ata1p1       64M part fat     /mnt/ata1p1
//   |-ata1p2       32M part (ntfs)
//   `-ata1p3       31M part (linux)
//   ahci0         128M disk         /mnt/ahci0
//   usb0          128M disk fat     /mnt/usb0
//
// Partitions come from each disk's MBR, so entries the kernel does not
// mount (another system's types) are listed too, with their table type in
// brackets. A device in /mnt that holds no readable volume shows its mount
// point with no FSTYPE (format it). -b prints sizes in bytes. A disk may be
// given as "ata1" or "/mnt/ata1"; a partition picks its disk.

#define LSBLK_NAME_WIDTH        12
#define LSBLK_SIZE_WIDTH        6       // "1023M", "9.9G"
#define LSBLK_BYTES_WIDTH       14      // -b: up to 99 TB
#define LSBLK_FSTYPE_WIDTH      7

static uint8_t lsblk_sector0[MBR_SECTOR_SIZE] __attribute__((aligned(16)));

static void lsblk_pad(size_t used, size_t width) {
    for (; used < width; used++)
        print(" ");
}

// "512", "1.5K", "120M", "2.0G": one decimal below 10, like lsblk
static void lsblk_human(uint64_t bytes, char* out) {
    static const char units[] = { 'B', 'K', 'M', 'G', 'T', 'P' };
    uint32_t unit = 0;
    uint64_t whole = bytes;
    uint64_t rest = 0;
    while (whole >= 1024 && unit + 1 < sizeof(units)) {
        rest  = whole % 1024;
        whole = whole / 1024;
        unit++;
    }

    if (unit == 0) {
        u64tos(bytes, out);
        return;
    }

    uint64_t tenths = (rest * 10 + 512) / 1024;
    if (tenths == 10) {
        whole++;
        tenths = 0;
    }
    u64tos(whole, out);
    if (whole < 10) {
        size_t length = strlen(out);
        out[length]     = '.';
        out[length + 1] = (char)('0' + tenths);
        out[length + 2] = '\0';
    }
    size_t length = strlen(out);
    out[length]     = units[unit];
    out[length + 1] = '\0';
}

// One row. `prefix` is the tree branch ("", "|-", "`-"). `fstype` and
// `mountpoint` may be empty.
static void lsblk_row(const char* prefix, const char* name, uint64_t bytes, bool in_bytes,
                      const char* type, const char* fstype, const char* mountpoint) {
    print(prefix);
    print(name);
    lsblk_pad(strlen(prefix) + strlen(name), LSBLK_NAME_WIDTH);

    char size[24];
    if (in_bytes)
        u64tos(bytes, size);
    else
        lsblk_human(bytes, size);
    lsblk_pad(strlen(size), in_bytes ? LSBLK_BYTES_WIDTH : LSBLK_SIZE_WIDTH);
    print(size);
    print(" ");

    print(type);
    print(" ");

    if (mountpoint[0] == '\0') {
        print(fstype);
    } else {
        print(fstype);
        lsblk_pad(strlen(fstype), LSBLK_FSTYPE_WIDTH);
        print(" ");
        print(mountpoint);
    }
    print("\n");
}

// Filesystem and mount point of the device called `name`: blank when it is
// not in /mnt, only the mount point when it holds no readable volume
static void lsblk_mount_of(const char* name, char fstype[VFS_FS_NAME_MAX],
                           char mountpoint[8 + BLOCK_NAME_MAX]) {
    fstype[0] = '\0';
    mountpoint[0] = '\0';

    char path[8 + BLOCK_NAME_MAX];
    strcpy(path, "/mnt/");
    strncat(path, name, BLOCK_NAME_MAX - 1);

    VfsDeviceInfo info;
    if (!vfs_device_get_info(path, &info))
        return;
    strcpy(mountpoint, path);
    if (info.formatted) {
        strncpy(fstype, info.filesystem, VFS_FS_NAME_MAX - 1);
        fstype[VFS_FS_NAME_MAX - 1] = '\0';
    }
}

static const char* lsblk_mbr_type_name(uint8_t type) {
    switch (type) {
        case MBR_TYPE_BASEFS:            return "basefs";
        case MBR_TYPE_GPT_PROTECTIVE:    return "gpt";
        case 0x0B: case 0x0C:            return "fat32";
        case 0x04: case 0x06: case 0x0E: return "fat16";
        case 0x01:                       return "fat12";
        case 0x05: case 0x0F:            return "extended";
        case 0x07:                       return "ntfs";
        case 0x82:                       return "swap";
        case 0x83:                       return "linux";
        default:                         return "other";
    }
}

// What a disk is: whether it holds a partition table, and its entries
struct LsblkDisk {
    char     name[BLOCK_NAME_MAX];
    uint64_t bytes;
    bool     readable;
    uint8_t  count;
    struct MbrPartition table[MBR_PARTITION_COUNT];
};

// Read a disk's table into `out`. Everything printed afterwards works from
// this copy and looks mounts up by name, so a disk unplugged part way
// through only shows as not mounted
static bool lsblk_read_disk(struct BlockDevice* disk, struct LsblkDisk* out) {
    strncpy(out->name, disk->name, BLOCK_NAME_MAX - 1);
    out->name[BLOCK_NAME_MAX - 1] = '\0';
    out->bytes    = block_capacity_bytes(disk);
    out->count    = 0;
    out->readable = disk->sector_count != 0 && block_read(disk, 0, 1, lsblk_sector0);
    if (!out->readable)
        return false;

    // A whole-disk volume can end in 55 AA too: it is not a table
    if (!block_mbr_parse(lsblk_sector0, disk->sector_count, out->table, &out->count) ||
        vfs_fs_probe(lsblk_sector0, disk->sector_size))
        out->count = 0;
    return true;
}

static void lsblk_print_disk(const struct LsblkDisk* disk, uint32_t sector_size, bool in_bytes) {
    char fstype[VFS_FS_NAME_MAX];
    char mountpoint[8 + BLOCK_NAME_MAX];
    lsblk_mount_of(disk->name, fstype, mountpoint);
    lsblk_row("", disk->name, disk->bytes, in_bytes, "disk", fstype, mountpoint);

    for (uint8_t i = 0; i < disk->count; i++) {
        const struct MbrPartition* p = &disk->table[i];

        char name[BLOCK_NAME_MAX + 4];
        strcpy(name, disk->name);
        size_t length = strlen(name);
        name[length]     = 'p';
        name[length + 1] = (char)('0' + p->number);
        name[length + 2] = '\0';

        lsblk_mount_of(name, fstype, mountpoint);
        if (mountpoint[0] == '\0') {
            // Not mounted: say what the table claims it holds
            strcpy(fstype, "(");
            strcat(fstype, p->usable ? lsblk_mbr_type_name(p->type) : "bad range");
            strcat(fstype, ")");
        }

        const char* branch = (i + 1 == disk->count) ? "`-" : "|-";
        lsblk_row(branch, name, p->count * (uint64_t)sector_size, in_bytes, "part", fstype, mountpoint);
    }
}

static int cmd_lsblk_run(int arg_count, char** args) {
    bool in_bytes = false;
    const char* wanted[16];             // At most 16 words reach a command
    int wanted_count = 0;

    for (int i = 0; i < arg_count; i++) {
        const char* arg = args[i];
        if (strcmp(arg, "-b") == 0) {
            in_bytes = true;
            continue;
        }
        if (arg[0] == '-') {
            print("Usage: lsblk [-b] [disk...]\n");
            return 1;
        }
        if (strncmp(arg, "/mnt/", 5) == 0)
            arg += 5;

        struct BlockDevice* device = block_find(arg);
        if (device == NULL) {
            print("No such device: ");
            print(arg);
            print("\n");
            return 1;
        }
        struct BlockDevice* parent = block_partition_parent(device);
        wanted[wanted_count++] = (parent != NULL) ? parent->name : device->name;
    }

    // Snapshot the disks: a partition is listed under its disk, never alone
    struct BlockDevice* disks[BLOCK_DEVICE_MAX];
    uint32_t disk_count = 0;
    uint32_t total = block_device_count();
    for (uint32_t i = 0; i < total && disk_count < BLOCK_DEVICE_MAX; i++) {
        struct BlockDevice* device = block_get(i);
        if (device == NULL || block_partition_parent(device) != NULL)
            continue;
        if (wanted_count > 0) {
            bool match = false;
            for (int w = 0; w < wanted_count; w++)
                match = match || strcmp(wanted[w], device->name) == 0;
            if (!match)
                continue;
        }
        disks[disk_count++] = device;
    }

    if (disk_count == 0) {
        print("No block devices\n");
        return 0;
    }

    print("NAME");
    lsblk_pad(4, LSBLK_NAME_WIDTH);
    lsblk_pad(4, in_bytes ? LSBLK_BYTES_WIDTH : LSBLK_SIZE_WIDTH);
    print("SIZE TYPE FSTYPE  MOUNTPOINT\n");

    int result = 0;
    for (uint32_t i = 0; i < disk_count; i++) {
        struct LsblkDisk disk;
        uint32_t sector_size = disks[i]->sector_size;
        if (!lsblk_read_disk(disks[i], &disk)) {
            lsblk_row("", disk.name, disk.bytes, in_bytes, "disk", "(unreadable)", "");
            result = 2;
            continue;
        }
        lsblk_print_disk(&disk, sector_size, in_bytes);
    }
    return result;
}

int console_cmd_lsblk(int arg_count, char** args) {
    int result = cmd_lsblk_run(arg_count, args);
    console_command_end();
    return result;
}

#endif
