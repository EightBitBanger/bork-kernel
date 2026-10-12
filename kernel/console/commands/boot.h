#ifndef CONSOLE_CMD_BOOT_H
#define CONSOLE_CMD_BOOT_H

#include <stdint.h>
#include <stdbool.h>

#include <kernel/kernel.h>
#include <kernel/util/string.h>

#include <kernel/block/block.h>
#include <kernel/block/partition.h>
#include <kernel/fs/fs.h>
#include <kernel/vfs/vfs.h>

#include <kernel/boot/x86/boot_image.h>
#include <kernel/arch/x86/bus/pci/pci.h>

#include <kernel/resources/resource_manager.h>

#include <kernel/programs/user_test/hello_elf.h>

#ifdef ADD_IMAGE_LIB
#include <kernel/dwm/dwm.h>   // dwm_builtin_images, dwm_get_image_directory
#endif

// Initial system setup / installation
//
//   boot install <disk>          format the disk to a blank file system and install the kernel
//   boot repair <disk> [force]   recreate missing system folders and system files (DWM images),
//                                fix their permissions, and check the boot area
//   boot update <disk>           replace the boot loader and kernel only, keeping the
//                                partition table and everything in the partition
//
// <disk> may be given as "ata1" or "/mnt/ata1". repair and update also take
// a partition ("ata1p1"): repair works on that partition, update on the
// disk it belongs to. A whole disk given to repair means its first partition.
//
// The install / update source is the boot area stage 2 loaded this kernel
// from (see boot_image.h), so it works when booted from a USB stick the
// kernel itself cannot read.
//
// Everything here runs on the console thread, one command at a time, so
// the static sector buffers below are never shared.

// Sector 0 layout (MBR)
#define BOOT_MBR_CODE_SIZE      0x1B8   // Boot code: everything before the disk signature
#define BOOT_MBR_TABLE          0x1BE
#define BOOT_MBR_ENTRY_SIZE     16
#define BOOT_MBR_ENTRIES        4
#define BOOT_MBR_SIGNATURE      0x1FE   // 0x55 0xAA

// Sectors read per request when comparing the boot area
#define BOOT_IO_SECTORS         8

// Static: mapped, physically stable memory a DMA driver (AHCI) can use
static uint8_t boot_io_buffer[BOOT_IO_SECTORS * MBR_SECTOR_SIZE] __attribute__((aligned(16)));
static uint8_t boot_mbr[MBR_SECTOR_SIZE] __attribute__((aligned(16)));

// System folders, relative to a device root. Parents come before children.
static const char* const boot_system_directories[] = {
    "/bin",
    "/sys",
    "/sys/images",
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

// Write the compiled-in images to <image_dir>/<name>.
//
// Without `overwrite`, files that already exist are left alone, so an image
// edited on disk is never replaced by the built-in copy. With it, existing
// files are removed and written again (repair force). A failed save is not
// fatal: the DWM falls back to the built-in copy.
static void boot_export_images_to(const char* image_dir, bool overwrite, BootExportStats* stats) {
    BootExportStats local = { 0, 0, 0 };
    if (stats == NULL)
        stats = &local;

#ifdef ADD_IMAGE_LIB
    if (!vfs_directory_check(image_dir) && !vfs_mkdir(image_dir)) {
        stats->failed += dwm_builtin_image_count;
        return;
    }

    for (uint32_t i = 0; i < dwm_builtin_image_count; i++) {
        const struct DWMBuiltinImage* image = &dwm_builtin_images[i];

        char image_path[256];
        if (!boot_path(image_path, sizeof(image_path), image_dir, "/") ||
            strlen(image_path) + strlen(image->name) + 1 > sizeof(image_path)) {
            stats->failed++;
            continue;
        }
        strncat(image_path, image->name, sizeof(image_path) - strlen(image_path) - 1);

        if (vfs_exists(image_path)) {
            if (!overwrite) {
                stats->kept++;
                continue;
            }
            // Never remove a folder that happens to carry an image's name
            if (vfs_directory_check(image_path) || !vfs_remove(image_path)) {
                stats->failed++;
                continue;
            }
        }

        resource_save(image_path, RESOURCE_TYPE_SPRITE, image->sprite);

        if (vfs_exists(image_path))
            stats->written++;
        else
            stats->failed++;
    }
#else
    (void)image_dir;
    (void)overwrite;
#endif
}

// The original "boot images": images into the current home device
static void boot_export_images(void) {
    struct LocalPaths paths;
    kernel_get_local_paths(&paths);

    // No home device: nowhere to save, the DWM uses the built-in images
    if (paths.home[0] == '\0')
        return;

    char sys_dir[128];
    if (!boot_path(sys_dir, sizeof(sys_dir), paths.home, "/sys"))
        return;

    if (!vfs_directory_check(sys_dir) && !vfs_mkdir(sys_dir))
        return;

#ifdef ADD_IMAGE_LIB
    char image_dir[128];
    if (dwm_get_image_directory(image_dir, sizeof(image_dir)))
        boot_export_images_to(image_dir, false, NULL);
#endif
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
                print(dir);
                print(" is a file, not a folder (boot repair <disk> force replaces it)\n");
                stats->failed++;
                continue;
            }
            if (!vfs_remove(dir)) {
                print("Could not remove the file ");
                print(dir);
                print("\n");
                stats->failed++;
                continue;
            }
        }

        if (!vfs_mkdir(dir)) {
            print("Could not create ");
            print(dir);
            print("\n");
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

        for (uint32_t s = 0; s < count; s++) {
            const uint8_t* want = image + (size_t)(lba + s) * MBR_SECTOR_SIZE;
            if (memcmp(boot_io_buffer + s * MBR_SECTOR_SIZE, want, MBR_SECTOR_SIZE) == 0)
                continue;
            (*changed)++;
            if (write && !block_write(disk, lba + s, 1, want))
                return BOOT_AREA_IO_ERROR;
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

static int boot_install(const char* disk_name) {
    uint32_t image_sectors = 0;
    const uint8_t* image = boot_get_image(&image_sectors);
    if (image == NULL) {
        print("No boot image: this kernel was not started by the stage 2 loader\n");
        return 1;
    }
    uint64_t data_start = VFS_PARTITION_START_BOOT;
    
    struct BlockDevice* disk = boot_find_device(disk_name);
    if (disk == NULL)
        return 2;
    if (block_partition_parent(disk) != NULL) {
        print("Give the whole disk, not a partition (ata1, not ata1p1)\n");
        return 2;
    }
    if (disk->sector_size != MBR_SECTOR_SIZE) {
        print("Only disks with 512-byte sectors are supported\n");
        return 2;
    }
    if (image_sectors > data_start) {
        print("Kernel too large for the reserved boot area\n");
        return 2;
    }
    if (disk->sector_count < data_start + 2048) {
        print("Disk too small (needs at least 5 MB)\n");
        return 2;
    }
    
    char part_name[BLOCK_NAME_MAX];
    boot_path(part_name, sizeof(part_name), disk->name, "p1");
    
    print("Install onto ");
    print(disk->name);
    print(" (");
    boot_print_mb(disk->sector_count);
    print(")\n  boot loader + kernel: ");
    print_int(image_sectors / 2);
    print(" KB\n  partition ");
    print(part_name);
    print(": ");
    boot_print_mb(disk->sector_count - data_start);
    print(", basefs\nEverything on ");
    print(disk->name);
    print(" is being erased.\n");
    
    // Nothing may stay mounted from this disk while it is rewritten
    VfsFormatResult result = vfs_device_release_disk(disk);
    if (result == VFS_FORMAT_BUSY) {
        print("Files on ");
        print(disk->name);
        print(" are open: close them first\n");
        return 3;
    }
    
    // Boot area and partition table
    print("Writing boot loader and kernel...\n");
    if (!block_install_boot_area(disk, image, image_sectors, MBR_TYPE_BASEFS, data_start)) {
        print("Write failed: the disk is not bootable\n");
        return 4;
    }
    
    // Mount the new partition and format it
    pci_mount_storage(disk);
    struct BlockDevice* part = block_find(part_name);
    if (part == NULL) {
        print("Partition did not appear\n");
        return 4;
    }
    
    print("Formatting ");
    print(part_name);
    print("...\n");
    struct FSDeviceContext* ctx = vfs_device_context(part);
    if (ctx == NULL ||
        vfs_device_format_context(ctx, fs_device_max_capacity(part), FS_MIN_SECTOR_SIZE,
                                false, NULL) != VFS_FORMAT_OK) {
        print("Format failed\n");
        return 4;
    }
    
    // Directory layout and system files (same as repair on a blank volume)
    char root[64];
    boot_path(root, sizeof(root), "/mnt/", part_name);
    
    BootLayoutStats layout;
    if (!boot_make_layout(root, false, &layout))
        return 5;
    
    char image_dir[128];
    boot_path(image_dir, sizeof(image_dir), root, "/sys/images");
    boot_export_images_to(image_dir, false, NULL);
    
    if (!vfs_sync_all()) {
        print("Warning: flushing a disk failed\n");
    }
    
    // Binary files
    char bin_dir[128];
    boot_path(bin_dir, sizeof(bin_dir), root, "/bin/test");
    
    hello_elf_drop(bin_dir);
    
    print("Installed. Remove the boot media and restart: ");
    print(part_name);
    print(" becomes home.\n");
    return 0;
}

//
// boot update
//

static int boot_update(const char* disk_name) {
    uint32_t image_sectors = 0;
    const uint8_t* image = boot_get_image(&image_sectors);
    if (image == NULL) {
        print("No boot image: this kernel was not started by the stage 2 loader\n");
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
        print("Only disks with 512-byte sectors are supported\n");
        return 2;
    }

    print("Update the boot loader and kernel on ");
    print(disk->name);
    print("\n  boot loader + kernel: ");
    print_int(image_sectors / 2);
    print(" KB\nThe partition table and the data partition are not touched.\n");

    // Only sectors in front of the first partition are written, which no
    // mount covers, so nothing has to be released or unmounted
    uint32_t changed = 0;
    BootAreaResult result = boot_area_sync(disk, image, image_sectors, true, &changed);

    switch (result) {
        case BOOT_AREA_NO_TABLE:
            print(disk->name);
            print(" has no system partition table: use boot install\n");
            return 3;
        case BOOT_AREA_TOO_SMALL:
            print("Kernel too large for the boot area in front of the partition on ");
            print(disk->name);
            print(" (boot install sets up a larger one, erasing the disk)\n");
            return 3;
        case BOOT_AREA_IO_ERROR:
            print("Disk I/O failed. If writing had started, ");
            print(disk->name);
            print(" may not boot: run boot update again before restarting\n");
            return 4;
        default:
            break;
    }

    if (changed == 0) {
        print("Already up to date\n");
        return 0;
    }

    print("Updated: ");
    boot_print_count(" sectors rewritten\n", changed);
    return 0;
}

//
// boot repair
//

static void boot_repair_check_area(struct BlockDevice* disk) {
    print("  boot area:   ");

    if (disk == NULL) {
        print("not checked (not a partition of a boot disk)\n");
        return;
    }

    uint32_t image_sectors = 0;
    const uint8_t* image = boot_get_image(&image_sectors);
    if (image == NULL) {
        print("not checked (this kernel was not started by the stage 2 loader)\n");
        return;
    }

    uint32_t differ = 0;
    switch (boot_area_sync(disk, image, image_sectors, false, &differ)) {
        case BOOT_AREA_OK:
            if (differ == 0) {
                print("matches the running kernel\n");
            } else {
                boot_print_count(" sectors differ from the running kernel (boot update ", differ);
                print(disk->name);
                print(" replaces them)\n");
            }
            break;
        case BOOT_AREA_NO_TABLE:
            print("no system partition table on ");
            print(disk->name);
            print("\n");
            break;
        case BOOT_AREA_TOO_SMALL:
            print("the running kernel does not fit in front of the partition\n");
            break;
        default:
            print("could not be read\n");
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
        print(" holds no readable file system: nothing to repair (boot install erases the disk and starts over)\n");
        return 3;
    }

    print("Repairing ");
    print(root);
    print(force ? " (force)\n" : "\n");

    // Folders first: the images go into one of them
    BootLayoutStats layout;
    boot_make_layout(root, force, &layout);

    print("  folders:     ");
    boot_print_count(" created, ", layout.created);
    boot_print_count(" permissions fixed, ", layout.fixed);
    boot_print_count(" failed\n", layout.failed);

    // System files
    BootExportStats images = { 0, 0, 0 };
    print("  images:      ");
#ifdef ADD_IMAGE_LIB
    char image_dir[128];
    boot_path(image_dir, sizeof(image_dir), root, "/sys/images");
    if (vfs_directory_check(image_dir)) {
        boot_export_images_to(image_dir, force, &images);
        boot_print_count(" written, ", images.written);
        boot_print_count(" kept, ", images.kept);
        boot_print_count(" failed\n", images.failed);
    } else {
        print("skipped (no images folder)\n");
        images.failed = 1;
    }
#else
    print("skipped (kernel built without the image library)\n");
#endif

    // Binary files
    char bin_dir[128];
    boot_path(bin_dir, sizeof(bin_dir), root, "/bin/test");
    
    hello_elf_drop(bin_dir);
    
    // Report only: the disk may intentionally hold another kernel version
    boot_repair_check_area(disk);

    bool synced = vfs_sync_all();
    if (!synced)
        print("Warning: flushing a disk failed\n");

    if (layout.failed > 0 || images.failed > 0 || !synced) {
        print("Repair incomplete\n");
        return 5;
    }

    print("Repaired\n");
    return 0;
}

//
// Command
//

static const char* msg_boot_usage =
    "Usage: boot install <disk>          install the system onto <disk> (ata0, ahci0, ...), erasing it\n"
    "       boot repair <disk> [force]   restore missing system folders and files on <disk>\n"
    "                                    (force also replaces files in the way and rewrites every image)\n"
    "       boot update <disk>           replace the boot loader and kernel, keeping all data\n";

int console_cmd_boot(int arg_count, char** args) {
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
        if (arg_count == 3 && strcmp(args[2], "force") == 0)
            return boot_repair(args[1], true);
    }

    // TODO "boot registry": initiate the registry as well
    print(msg_boot_usage);
    return 1;
}

#endif
