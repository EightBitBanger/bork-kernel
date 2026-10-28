#ifndef CONSOLE_CMD_BOOT_H
#define CONSOLE_CMD_BOOT_H

#include <stdint.h>
#include <stdbool.h>

#include <kernel/kernel.h>
#include <kernel/util/string.h>

#include <kernel/block/block.h>
#include <kernel/block/partition.h>
#include <kernel/fs/fs.h>
#include <kernel/fs/fs_type.h>
#include <kernel/vfs/vfs.h>
#include <kernel/fs/format.h>

#include <kernel/boot/x86/boot_image.h>
#include <kernel/arch/x86/bus/pci/pci.h>

// Assets ("boot install" / "boot repair" write these to disk): the archive
// stage 2 loaded
#include <kernel/console/commands/boot_assets.h>
#include <kernel/console/commands/boot_registry.h>

// Initial system setup / installation
//
//   boot install <disk>          install the boot loader and kernel in front of the disk's
//                                partitions and the system onto its boot partition, which
//                                is formatted. The table comes from fdisk; <disk> may also
//                                be a partition ("ata1p2") to install onto that one
//   boot repair <disk> [-f]      recreate missing system folders and system files (programs, DWM images),
//                                fix their permissions, add missing registry defaults (the desktop's
//                                context menus, boot_registry.h), and check the boot area
//   boot update <disk>           replace the boot loader and kernel only, keeping the
//                                partition table and everything in the partition
//
// <disk> may be given as "ata1" or "/mnt/ata1". All three also take a
// partition ("ata1p1"): install and repair work on that partition, update
// on the disk it belongs to. A whole disk given to install means its active
// basefs partition (else the first basefs one), to repair its first
// partition.
//
// Setting up a new boot disk:
//
//   format ata1 -q       erase it all, boot sector and partition table included
//   fdisk ata1 new       one basefs partition, with room for the boot area
//   boot install ata1    boot sector, stage 2 and kernel, then the system
//
// The install / update source is the boot area stage 2 loaded this kernel
// from (see boot_image.h), so it works when booted from a USB stick the
// kernel itself cannot read. The asset archive the system files are
// extracted from is part of that boot area (behind the kernel file), so an
// installed disk carries it too and "boot repair" works when booted from
// it, and "boot update" refreshes it along with the kernel.
//
// Everything here runs on the console thread, one command at a time, so
// the static sector buffers below are never shared.
//
// Output style: a one-line header, indented detail lines with labels padded
// to the same width, a blank line between stages, and a one-line result.

// Sector 0 layout (MBR)
#define BOOT_MBR_CODE_SIZE      0x1B8   // Boot code: everything before the disk signature
#define BOOT_MBR_TABLE          0x1BE
#define BOOT_MBR_ENTRY_SIZE     16
#define BOOT_MBR_ENTRIES        4
#define BOOT_MBR_SIGNATURE      0x1FE   // 0x55 0xAA

// Sectors read (and, where they differ, written) per request when syncing
// the boot area. 64 is the most a USB stick takes per command.
#define BOOT_IO_SECTORS         64

// Static: mapped, physically stable memory a DMA driver (AHCI) can use
static uint8_t boot_io_buffer[BOOT_IO_SECTORS * MBR_SECTOR_SIZE] __attribute__((aligned(16)));
static uint8_t boot_mbr[MBR_SECTOR_SIZE] __attribute__((aligned(16)));

// System folders, relative to a device root. Parents come before children.
// Only folders no asset lands in: the folders the archive's files go into
// (sys, bin, fonts, ...) are made as the files are dropped (boot_assets.h).
static const char* const boot_system_directories[] = {
    "/usr",
    "/usr/desktop"
};

#define BOOT_SYSTEM_DIRECTORY_COUNT \
    (sizeof(boot_system_directories) / sizeof(boot_system_directories[0]))

//
// Small helpers
//

// "<base><suffix>" into out. False (out empty) if it does not fit.
static bool boot_path(char* out, size_t size, const char* base, const char* suffix) {
    memset(out, '\0', size);
    if (strlen(base) + strlen(suffix) + 1 > size)
        return false;
    strncpy(out, base, size - 1);
    strncat(out, suffix, size - strlen(out) - 1);
    return true;
}

static void boot_print_mb(uint64_t sectors) {
    print_int((uint32_t)(sectors / 2048));
    print(" MB");
}

static void boot_print_count(const char* label, uint32_t value) {
    print_int(value);
    print(label);
}

// "  <label>" padded so every detail line's value starts in the same column
#define BOOT_LABEL_WIDTH 12

static void boot_print_label(const char* label) {
    print("  ");
    print(label);
    for (size_t i = strlen(label); i < BOOT_LABEL_WIDTH; i++)
        print(" ");
}

// "N created, N fixed, N failed" style line
static void boot_print_stats(const char* label,
                             uint32_t a, const char* a_name,
                             uint32_t b, const char* b_name,
                             uint32_t failed) {
    boot_print_label(label);
    print_int(a);
    print(a_name);
    print(", ");
    print_int(b);
    print(b_name);
    if (failed > 0) {
        print(", ");
        boot_print_count(" failed", failed);
    }
    print("\n");
}

// Accept "ata1" or "/mnt/ata1"
static const char* boot_strip_mnt(const char* name) {
    if (strncmp(name, "/mnt/", 5) == 0)
        return name + 5;
    return name;
}

static struct BlockDevice* boot_find_device(const char* name) {
    name = boot_strip_mnt(name);
    struct BlockDevice* device = block_find(name);
    if (device == NULL) {
        print("No disk named ");
        print(name);
        print("\n");
    }
    return device;
}

// The boot area this kernel came from, or NULL. *sectors = its length.
static const uint8_t* boot_get_image(uint32_t* sectors) {
    uint32_t bytes = 0;
    const uint8_t* image = boot_image_get(&bytes);
    if (image == NULL || bytes < 2 * MBR_SECTOR_SIZE || bytes % MBR_SECTOR_SIZE != 0)
        return NULL;
    *sectors = bytes / MBR_SECTOR_SIZE;
    return image;
}

//
// System files
//

typedef struct {
    uint32_t written;
    uint32_t kept;      // Already there and left alone
    uint32_t failed;
} BootExportStats;

// Extract the asset archive (ELF executables, e.g. the DWM, maps, fonts,
// images)
// to <root>/<path>, making any missing folders on the way. `stats` may be
// NULL. False if there is no archive (*missing set) or anything failed.
static bool boot_export_files(const char* root, bool overwrite, BootExportStats* stats, bool* missing) {
    BootExportStats local = { 0, 0, 0 };
    if (stats == NULL)
        stats = &local;

    BootAssetStats a = { 0, 0, 0 };
    *missing = !boot_drop_archive(root, overwrite, &a);
    stats->written += a.written;
    stats->kept    += a.kept;
    stats->failed  += a.failed;
    return !*missing && a.failed == 0;
}

//
// Folder layout
//

typedef struct {
    uint32_t created;
    uint32_t fixed;     // Permissions restored
    uint32_t failed;
} BootLayoutStats;

// Give `path` read and write permission if it lacks either. True if changed.
static bool boot_ensure_rw(const char* path) {
    uint8_t perm = 0;
    if (!vfs_get_permissions(path, &perm))
        return false;
    uint8_t want = (uint8_t)(perm | VFS_PERMISSION_READ | VFS_PERMISSION_WRITE);
    if (want == perm)
        return false;
    return vfs_set_permissions(path, want);
}

static void boot_print_problem(const char* what, const char* path) {
    print("  ! ");
    print(what);
    print(path);
    print("\n");
}

// Make sure the device root and every system folder below it exist as
// folders the system can read and write. A file standing where a folder
// belongs is only removed with `force`. Returns false if anything is still
// missing afterwards.
static bool boot_make_layout(const char* root, bool force, BootLayoutStats* stats) {
    stats->created = 0;
    stats->fixed   = 0;
    stats->failed  = 0;

    // Every mkdir below needs a writable parent, starting with the root
    if (boot_ensure_rw(root))
        stats->fixed++;

    for (uint32_t i = 0; i < BOOT_SYSTEM_DIRECTORY_COUNT; i++) {
        char dir[128];
        if (!boot_path(dir, sizeof(dir), root, boot_system_directories[i])) {
            stats->failed++;
            continue;
        }

        if (vfs_directory_check(dir)) {
            if (boot_ensure_rw(dir))
                stats->fixed++;
            continue;
        }

        if (vfs_exists(dir)) {
            if (!force) {
                boot_print_problem("file in the way (use -f): ", dir);
                stats->failed++;
                continue;
            }
            if (!vfs_remove(dir)) {
                boot_print_problem("cannot remove ", dir);
                stats->failed++;
                continue;
            }
        }

        if (!vfs_mkdir(dir)) {
            boot_print_problem("cannot create ", dir);
            stats->failed++;
            continue;
        }
        stats->created++;
    }

    return stats->failed == 0;
}

//
// Boot area (sector 0 boot code, stage 2, kernel file)
//

typedef enum {
    BOOT_AREA_OK = 0,
    BOOT_AREA_NO_TABLE,     // No MBR signature or no basefs partition
    BOOT_AREA_TOO_SMALL,    // The image would run into the first partition
    BOOT_AREA_IO_ERROR,
} BootAreaResult;

static inline uint32_t boot_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Start of the lowest partition in an MBR, if the table is valid and holds
// a basefs partition (i.e. it is a disk "boot install" set up)
static bool boot_mbr_first_partition(const uint8_t* mbr, uint64_t* first) {
    if (mbr[BOOT_MBR_SIGNATURE] != 0x55 || mbr[BOOT_MBR_SIGNATURE + 1] != 0xAA)
        return false;

    bool found  = false;
    bool basefs = false;
    uint64_t lowest = 0;

    for (uint32_t i = 0; i < BOOT_MBR_ENTRIES; i++) {
        const uint8_t* entry = mbr + BOOT_MBR_TABLE + i * BOOT_MBR_ENTRY_SIZE;
        uint8_t  type  = entry[4];
        uint32_t start = boot_le32(entry + 8);
        uint32_t count = boot_le32(entry + 12);
        if (type == 0 || count == 0)
            continue;

        if (type == MBR_TYPE_BASEFS)
            basefs = true;
        if (!found || start < lowest)
            lowest = start;
        found = true;
    }

    if (!found || !basefs || lowest == 0)
        return false;
    *first = lowest;
    return true;
}

// Compare the boot area on `disk` with `image` and, with `write`, rewrite
// every sector that differs. Sector 0 keeps the disk's own signature and
// partition table; only its boot code comes from the image. Nothing at or
// past the first partition is ever touched. *changed counts the sectors
// that differ (or were rewritten).
//
// Stage 2 and the kernel are written first and sector 0 last. This is not
// atomic: if it is cut short the disk may not boot until it is run again.
static BootAreaResult boot_area_sync(struct BlockDevice* disk, const uint8_t* image, uint32_t image_sectors,
                                     bool write, uint32_t* changed) {
    *changed = 0;

    if (!block_read(disk, 0, 1, boot_mbr))
        return BOOT_AREA_IO_ERROR;

    uint64_t first_partition = 0;
    if (!boot_mbr_first_partition(boot_mbr, &first_partition))
        return BOOT_AREA_NO_TABLE;
    if ((uint64_t)image_sectors > first_partition)
        return BOOT_AREA_TOO_SMALL;

    for (uint32_t lba = 1; lba < image_sectors; ) {
        uint32_t count = image_sectors - lba;
        if (count > BOOT_IO_SECTORS)
            count = BOOT_IO_SECTORS;

        if (!block_read(disk, lba, count, boot_io_buffer))
            return BOOT_AREA_IO_ERROR;

        // Each run of differing sectors goes out in one write: on a blank
        // disk that is the whole chunk, not one command per sector
        for (uint32_t s = 0; s < count; ) {
            const uint8_t* want = image + (size_t)(lba + s) * MBR_SECTOR_SIZE;
            if (memcmp(boot_io_buffer + s * MBR_SECTOR_SIZE, want, MBR_SECTOR_SIZE) == 0) {
                s++;
                continue;
            }
            uint32_t run = 1;
            while (s + run < count &&
                   memcmp(boot_io_buffer + (s + run) * MBR_SECTOR_SIZE,
                          want + run * MBR_SECTOR_SIZE, MBR_SECTOR_SIZE) != 0)
                run++;
            *changed += run;
            if (write && !block_write(disk, lba + s, run, want))
                return BOOT_AREA_IO_ERROR;
            s += run;
        }
        lba += count;
    }

    // Sector 0: new boot code, the disk's own signature and table
    memcpy(boot_io_buffer, boot_mbr, MBR_SECTOR_SIZE);
    memcpy(boot_io_buffer, image, BOOT_MBR_CODE_SIZE);
    if (memcmp(boot_io_buffer, boot_mbr, MBR_SECTOR_SIZE) != 0) {
        (*changed)++;
        if (write && !block_write(disk, 0, 1, boot_io_buffer))
            return BOOT_AREA_IO_ERROR;
    }

    if (write && *changed > 0 && !block_flush(disk))
        return BOOT_AREA_IO_ERROR;
    return BOOT_AREA_OK;
}

//
// boot install
//

// Find the partition of the table on `disk` to install onto: the one at
// `start` when the user named a partition (start != 0), else the active
// basefs partition, else the first basefs one. NULL if there is none.
static const struct MbrPartition* boot_pick_partition(const struct MbrPartition* table, uint8_t count,
                                                      uint64_t start) {
    const struct MbrPartition* first = NULL;
    for (uint8_t i = 0; i < count; i++) {
        const struct MbrPartition* p = &table[i];
        if (start != 0) {
            if (p->start == start)
                return p;
            continue;
        }
        if (p->type != MBR_TYPE_BASEFS)
            continue;
        if (p->active)
            return p;
        if (first == NULL)
            first = p;
    }
    return first;
}

static void boot_print_no_table(const struct BlockDevice* disk) {
    print(disk->name);
    print(" has no basefs partition. Set one up first:\n  fdisk ");
    print(disk->name);
    print(" new\n");
}

static int boot_install(const char* disk_name) {
    uint32_t image_sectors = 0;
    const uint8_t* image = boot_get_image(&image_sectors);
    if (image == NULL) {
        print("No boot image (kernel not started by the boot loader)\n");
        return 1;
    }

    struct BlockDevice* device = boot_find_device(disk_name);
    if (device == NULL)
        return 2;

    // "ata1p2" means that partition of ata1
    struct BlockDevice* disk = block_partition_parent(device);
    uint64_t wanted_start = 0;
    if (disk != NULL)
        wanted_start = block_partition_start(device);
    else
        disk = device;

    if (disk->sector_size != MBR_SECTOR_SIZE) {
        print("Only 512-byte sectors are supported\n");
        return 2;
    }

    // The partition table fdisk wrote
    struct MbrPartition table[MBR_PARTITION_COUNT];
    uint8_t entries = 0;
    if (!block_read(disk, 0, 1, boot_mbr)) {
        print("Cannot read ");
        print(disk->name);
        print("\n");
        return 4;
    }
    if (!block_mbr_parse(boot_mbr, disk->sector_count, table, &entries) ||
        vfs_fs_probe(boot_mbr, disk->sector_size)) {
        boot_print_no_table(disk);
        return 2;
    }

    const struct MbrPartition* target = boot_pick_partition(table, entries, wanted_start);
    if (target == NULL) {
        boot_print_no_table(disk);
        return 2;
    }
    if (target->type != MBR_TYPE_BASEFS || !target->usable) {
        print("Partition ");
        print_int(target->number);
        print(target->usable ? " is not a basefs partition\n" : " does not fit the disk\n");
        return 2;
    }

    // Stage 2 and the kernel go in front of the first partition
    uint64_t first_start = target->start;
    for (uint8_t i = 0; i < entries; i++) {
        if (table[i].start < first_start)
            first_start = table[i].start;
    }
    if ((uint64_t)image_sectors > first_start) {
        print("Kernel too large for the boot area: it needs ");
        print_int(image_sectors);
        print(" sectors, the first partition starts at sector ");
        print_int((uint32_t)first_start);
        print("\nMake room: delete the partitions below it (fdisk ");
        print(disk->name);
        print(" del <n>), then fdisk ");
        print(disk->name);
        print(" new\n");
        return 2;
    }

    char part_name[BLOCK_NAME_MAX];
    char number[4] = { 'p', (char)('0' + target->number), '\0', '\0' };
    if (!boot_path(part_name, sizeof(part_name), disk->name, number)) {
        print("Disk name too long\n");
        return 2;
    }

    // Header
    print("Install onto ");
    print(disk->name);
    print(" (");
    boot_print_mb(disk->sector_count);
    print(")\n");

    boot_print_label("boot area");
    print_int(image_sectors / 2);
    print(" KB\n");

    boot_print_label(part_name);
    boot_print_mb(target->count);
    print(", basefs\n\n");

    print("Erasing all data on ");
    print(part_name);
    print("\n\n");

    // The installed disk must never pass for the disk this kernel booted
    // from (a clone has the same signature), or for a disk without one
    if (!block_mbr_renew_signature(disk, block_mbr_signature(image))) {
        print("Write failed, disk is not bootable\n");
        return 4;
    }

    // Stage 1 does not look at the boot flag, but other loaders and tools do
    if (!target->active) {
        for (uint8_t i = 0; i < entries; i++)
            table[i].active = (&table[i] == target);
        if (!block_mbr_write(disk, table, entries, true)) {
            print("Write failed, disk is not bootable\n");
            return 4;
        }
    }

    // Boot code into sector 0 (the table stays), stage 2 and the kernel
    // behind it. Only sectors in front of the first partition are written,
    // so mounts on the disk do not have to be released for this.
    print("Writing boot area...\n");
    uint32_t changed = 0;
    if (boot_area_sync(disk, image, image_sectors, true, &changed) != BOOT_AREA_OK) {
        print("\nWrite failed, disk is not bootable\n");
        return 4;
    }

    // The partition as a mounted device, in step with the table. A table
    // written by something else than fdisk has not been scanned yet.
    struct BlockDevice* part = block_find(part_name);
    if (part == NULL || block_partition_start(part) != target->start ||
        part->sector_count != target->count || vfs_device_context(part) == NULL) {
        if (vfs_device_release_disk(disk) == VFS_FORMAT_BUSY) {
            print("Files on ");
            print(disk->name);
            print(" are open, close them first\n");
            return 3;
        }
        block_partition_drop_all(disk);
        pci_mount_storage(disk);
        part = block_find(part_name);
    }
    struct FsMount* mount = (part != NULL) ? vfs_device_context(part) : NULL;
    if (mount == NULL) {
        print("\nPartition did not appear\n");
        return 4;
    }

    print("Formatting ");
    print(part_name);
    print("...\n");
    // basefs whatever the partition was driven as (a FAT volume left in a
    // basefs entry): the system folders need its security masks
    VfsFormatResult result = vfs_device_format_as(mount, &fs_type_basefs,
                                                  fs_type_basefs.max_capacity(part),
                                                  FS_MIN_SECTOR_SIZE, false, NULL);
    if (result == VFS_FORMAT_BUSY) {
        print("\nFiles on ");
        print(part_name);
        print(" are open, close them first\n");
        return 3;
    }
    if (result != VFS_FORMAT_OK) {
        print("\nFormat failed\n");
        return 4;
    }

    // Directory layout and system files (same as repair on a blank volume)
    print("Writing system files...\n");

    char root[64];
    boot_path(root, sizeof(root), "/mnt/", part_name);

    BootLayoutStats layout;
    if (!boot_make_layout(root, false, &layout)) {
        print("\nInstall incomplete\n");
        return 5;
    }

    // The asset archive (its folders are made as files are dropped)
    bool missing = false;
    if (!boot_export_files(root, false, NULL, &missing))
        print(missing ? "  ! no asset archive on the boot media, system files not installed\n"
                      : "  ! some system files missing\n");

    // Registry defaults (the desktop's context menus) into the new sys/hkr
    BootRegistryStats reg;
    if (!boot_registry_defaults(root, false, &reg))
        print("  ! registry defaults not written\n");

    if (!vfs_sync_all())
        print("  ! disk flush failed\n");

    print("\nInstallation complete. Remove the boot media and restart.\n");
    return 0;
}

//
// boot update
//

static int boot_update(const char* disk_name) {
    uint32_t image_sectors = 0;
    const uint8_t* image = boot_get_image(&image_sectors);
    if (image == NULL) {
        print("No boot image (kernel not started by the boot loader)\n");
        return 1;
    }

    struct BlockDevice* disk = boot_find_device(disk_name);
    if (disk == NULL)
        return 2;

    // "ata1p1" means the disk it lives on
    struct BlockDevice* parent = block_partition_parent(disk);
    if (parent != NULL)
        disk = parent;

    if (disk->sector_size != MBR_SECTOR_SIZE) {
        print("Only 512-byte sectors are supported\n");
        return 2;
    }

    print("Updated ");
    print(disk->name);
    print(" (");
    print_int(image_sectors / 2);
    print(" KB)\n");

    // Only sectors in front of the first partition are written, which no
    // mount covers, so nothing has to be released or unmounted
    uint32_t changed = 0;
    BootAreaResult result = boot_area_sync(disk, image, image_sectors, true, &changed);

    switch (result) {
        case BOOT_AREA_NO_TABLE:
            print("No system partition table, use fdisk and boot install\n");
            return 3;
        case BOOT_AREA_TOO_SMALL:
            print("Kernel too large for the boot area\n");
            print("(fdisk new and boot install make room, but erase the disk)\n");
            return 3;
        case BOOT_AREA_IO_ERROR:
            print("Disk I/O failed\n");
            print("Run boot update again before restarting\n");
            return 4;
        default:
            break;
    }

    if (changed == 0) {
        print("Already up to date\n");
        return 0;
    }

    boot_print_count(" sectors rewritten\n", changed);
    return 0;
}

//
// boot repair
//

static void boot_repair_check_area(struct BlockDevice* disk) {
    boot_print_label("boot area");

    if (disk == NULL) {
        print("not checked (not on a boot disk)\n");
        return;
    }

    uint32_t image_sectors = 0;
    const uint8_t* image = boot_get_image(&image_sectors);
    if (image == NULL) {
        print("not checked (no boot image)\n");
        return;
    }

    uint32_t differ = 0;
    switch (boot_area_sync(disk, image, image_sectors, false, &differ)) {
        case BOOT_AREA_OK:
            if (differ == 0) {
                print("up to date\n");
            } else {
                boot_print_count(" sectors differ (boot update ", differ);
                print(disk->name);
                print(")\n");
            }
            break;
        case BOOT_AREA_NO_TABLE:
            print("no system partition table\n");
            break;
        case BOOT_AREA_TOO_SMALL:
            print("too small for this kernel\n");
            break;
        default:
            print("read failed\n");
            break;
    }
}

static int boot_repair(const char* name, bool force) {
    struct BlockDevice* device = boot_find_device(name);
    if (device == NULL)
        return 2;

    // A whole disk with a partition table: the system lives in its first
    // partition. A whole-disk volume (no table) is repaired as it is.
    struct BlockDevice* disk = block_partition_parent(device);
    if (disk == NULL) {
        char part_name[BLOCK_NAME_MAX];
        if (boot_path(part_name, sizeof(part_name), device->name, "p1")) {
            struct BlockDevice* part = block_find(part_name);
            if (part != NULL) {
                disk   = device;
                device = part;
            }
        }
    }

    char root[64];
    boot_path(root, sizeof(root), "/mnt/", device->name);

    VfsDeviceInfo info;
    if (!vfs_device_get_info(root, &info)) {
        print(device->name);
        print(" is not mounted\n");
        return 3;
    }
    if (!info.formatted) {
        print(device->name);
        print(" has no file system (use boot install)\n");
        return 3;
    }

    print("Repairing ");
    print(root);
    print(force ? " (-f)\n\n" : "\n\n");

    // Folders no asset lands in (the asset folders are made by the drops)
    BootLayoutStats layout;
    boot_make_layout(root, force, &layout);
    boot_print_stats("folders", layout.created, " created",
                     layout.fixed, " fixed", layout.failed);

    // System files: the asset archive (ELF executables, e.g. the DWM, their
    // data files, fonts and images)
    BootExportStats files = { 0, 0, 0 };
    bool missing = false;
    bool files_ok = boot_export_files(root, force, &files, &missing);
    if (missing) {
        boot_print_label("files");
        print("not checked (no asset archive)\n");
    } else {
        boot_print_stats("files", files.written, " written",
                         files.kept, " kept", files.failed);
    }

    // Registry defaults: menus that are missing or empty (all of them with -f)
    BootRegistryStats reg;
    bool reg_ok = boot_registry_defaults(root, force, &reg);
    boot_print_stats("registry", reg.written, " written",
                     reg.kept, " kept", reg.failed);

    // Report only: the disk may intentionally hold another kernel version
    boot_repair_check_area(disk);

    bool synced = vfs_sync_all();
    if (!synced)
        print("  ! disk flush failed\n");

    if (layout.failed > 0 || !files_ok || !reg_ok || !synced) {
        print("\nRepair incomplete\n");
        return 5;
    }

    print("\nRepair complete\n");
    return 0;
}

//
// Command
//

static const char* msg_boot_usage =
    "Usage:\n"
    "  boot install <disk>          install the boot sector and kernel, then format\n"
    "                               the boot partition (fdisk) and install the system\n"
    "  boot repair <disk> [-f]      restore system folders, files and registry\n"
    "                               defaults (-f replaces existing ones)\n"
    "  boot update <disk>           update the boot loader and kernel only\n";

static int cmd_boot_run(int arg_count, char** args) {
    if (arg_count < 1) {
        print(msg_boot_usage);
        return 1;
    }

    if (arg_count >= 2 && strcmp(args[0], "install") == 0)
        return boot_install(args[1]);

    if (arg_count >= 2 && strcmp(args[0], "update") == 0)
        return boot_update(args[1]);

    if (strcmp(args[0], "repair") == 0) {
        if (arg_count == 2)
            return boot_repair(args[1], false);
        if (arg_count == 3 && strcmp(args[2], "-f") == 0)
            return boot_repair(args[1], true);
    }

    // TODO "boot registry": initiate the registry as well
    print(msg_boot_usage);
    return 1;
}

int console_cmd_boot(int arg_count, char** args) {
    // Never while a program formats a disk (SYS_FORMAT), or the reverse
    if (!format_lock_try()) {
        print("Another format is running, try again when it is done\n");
        console_command_end();
        return 5;
    }
    int result = cmd_boot_run(arg_count, args);
    format_lock_release();
    console_command_end();
    return result;
}

#endif
