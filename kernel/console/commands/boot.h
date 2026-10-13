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
//
// Output style: a one-line header, indented detail lines with labels padded
// to the same width, a blank line between stages, and a one-line result.

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
    "/bin/test",        // Built-in test programs land here
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

// Drop the built-in test programs into <root>/bin/test. That folder is
// created by boot_make_layout (it is in boot_system_directories), so the
// files have a parent to live in; dropper_all only writes them. Returns true
// only if every program was written.
static bool boot_export_programs(const char* root) {
    char bin_dir[128];
    if (!boot_path(bin_dir, sizeof(bin_dir), root, "/bin/test"))
        return false;
    return dropper_all(bin_dir);
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
                boot_print_problem("file in the way (use force): ", dir);
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
        print("No boot image (kernel not started by the boot loader)\n");
        return 1;
    }
    uint64_t data_start = VFS_PARTITION_START_BOOT;

    struct BlockDevice* disk = boot_find_device(disk_name);
    if (disk == NULL)
        return 2;
    if (block_partition_parent(disk) != NULL) {
        print("Need a whole disk (ata1, not ata1p1)\n");
        return 2;
    }
    if (disk->sector_size != MBR_SECTOR_SIZE) {
        print("Only 512-byte sectors are supported\n");
        return 2;
    }
    if (image_sectors > data_start) {
        print("Kernel too large for the boot area\n");
        return 2;
    }
    if (disk->sector_count < data_start + 2048) {
        print("Disk too small (5 MB minimum)\n");
        return 2;
    }

    char part_name[BLOCK_NAME_MAX];
    boot_path(part_name, sizeof(part_name), disk->name, "p1");

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
    boot_print_mb(disk->sector_count - data_start);
    print(", basefs\n\n");

    print("Erasing all data on ");
    print(disk->name);
    print("\n\n");

    // Nothing may stay mounted from this disk while it is rewritten
    VfsFormatResult result = vfs_device_release_disk(disk);
    if (result == VFS_FORMAT_BUSY) {
        print("Files on ");
        print(disk->name);
        print(" are open, close them first\n");
        return 3;
    }

    // Boot area and partition table
    print("Writing boot area...\n");
    if (!block_install_boot_area(disk, image, image_sectors, MBR_TYPE_BASEFS, data_start)) {
        print("\nWrite failed, disk is not bootable\n");
        return 4;
    }

    // Mount the new partition and format it
    pci_mount_storage(disk);
    struct BlockDevice* part = block_find(part_name);
    if (part == NULL) {
        print("\nPartition did not appear\n");
        return 4;
    }

    print("Formatting ");
    print(part_name);
    print("...\n");
    struct FSDeviceContext* ctx = vfs_device_context(part);
    if (ctx == NULL ||
        vfs_device_format_context(ctx, fs_device_max_capacity(part), FS_MIN_SECTOR_SIZE,
                                false, NULL) != VFS_FORMAT_OK) {
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

    char image_dir[128];
    boot_path(image_dir, sizeof(image_dir), root, "/sys/images");
    boot_export_images_to(image_dir, false, NULL);

    if (!vfs_sync_all())
        print("  ! disk flush failed\n");

    // Binary files (the /bin/test folder was just made by boot_make_layout)
    if (!boot_export_programs(root))
        print("  ! some test programs missing from /bin/test\n");

    print("\nInstalled. Remove the boot media and restart.\n");
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
            print("No system partition table, use boot install\n");
            return 3;
        case BOOT_AREA_TOO_SMALL:
            print("Kernel too large for the boot area\n");
            print("(boot install makes room, but erases the disk)\n");
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
    print(force ? " (force)\n\n" : "\n\n");

    // Folders first: the images go into one of them
    BootLayoutStats layout;
    boot_make_layout(root, force, &layout);
    boot_print_stats("folders", layout.created, " created",
                     layout.fixed, " fixed", layout.failed);

    // System files
    BootExportStats images = { 0, 0, 0 };
#ifdef ADD_IMAGE_LIB
    char image_dir[128];
    boot_path(image_dir, sizeof(image_dir), root, "/sys/images");
    if (vfs_directory_check(image_dir)) {
        boot_export_images_to(image_dir, force, &images);
        boot_print_stats("images", images.written, " written",
                         images.kept, " kept", images.failed);
    } else {
        boot_print_label("images");
        print("skipped (no images folder)\n");
        images.failed = 1;
    }
#else
    boot_print_label("images");
    print("skipped (no image library)\n");
#endif

    // Binary files
    bool programs_ok = boot_export_programs(root);
    boot_print_label("programs");
    print(programs_ok ? "ok\n" : "failed\n");

    // Report only: the disk may intentionally hold another kernel version
    boot_repair_check_area(disk);

    bool synced = vfs_sync_all();
    if (!synced)
        print("  ! disk flush failed\n");

    if (layout.failed > 0 || images.failed > 0 || !programs_ok || !synced) {
        print("\nRepair incomplete\n");
        return 5;
    }

    print("\nRepaired\n");
    return 0;
}

//
// Command
//

static const char* msg_boot_usage =
    "Usage:\n"
    "  boot install <disk>          erase <disk> and install the system\n"
    "  boot repair <disk> [force]   restore system folders and files\n"
    "                               (force replaces existing ones)\n"
    "  boot update <disk>           update the boot loader and kernel only\n";

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
