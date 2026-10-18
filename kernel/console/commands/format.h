#ifndef CONSOLE_CMD_FORMAT_H
#define CONSOLE_CMD_FORMAT_H

#include <stdint.h>
#include <stdbool.h>

#include <kernel/console/display.h>
#include <kernel/fs/fs.h>
#include <kernel/vfs/vfs.h>
#include <kernel/block/block.h>
#include <kernel/block/partition.h>

// Usage: format <device> [-q] [-k<KB> | -M<MB>] [-s<block size>]
//
//   <device>    block device to format: "ata1", "ahci0", "ata0p1" (or
//               "/mnt/ata1"). A disk with partitions is formatted through
//               its partitions, never as a whole.
//   -q          quick format: write the filesystem structures only
//   -k / -M     filesystem size; defaults to the current size, or the whole
//               device if it is unformatted
//   -s          allocation block size: 32, 64, 512 or 1024 bytes

static const char* msg_usage             = "Usage: format <device> [-q] [-k<KB> | -M<MB>] [-s<block size>]\n";
static const char* msg_no_device         = "No device named ";
static const char* msg_not_mounted       = " is not mounted\n";
static const char* msg_use_partition     = " has partitions: format ";
static const char* msg_bytes_total       = " bytes total\n";
static const char* msg_sector_size       = " sector size\n\n";
static const char* msg_formatting        = "Formatting ";
static const char* msg_ellipsis          = "...\n";
static const char* msg_unknown_arg       = "Unknown argument '";
static const char* msg_missing_val       = "Missing value for argument '";
static const char* msg_invalid_val       = "Invalid value for argument '";
static const char* msg_too_large         = "Capacity exceeds the device (max ";
static const char* msg_bytes_close       = " bytes)\n";
static const char* msg_busy              = "Device busy: close open files on it first\n";
static const char* msg_failed            = "Format failed\n";
static const char* msg_quote_newline     = "'\n";
static const char* msg_percent           = "%";
static const char* msg_init_progress     = "0%";
static const char* msg_quick_progress    = "50%";
static const char* msg_final_progress    = "100%\n\n";

static void format_print_progress(uint32_t percent) {
    display_cursor_set_position(0);
    print_int(percent);
    print(msg_percent);
}

static int format_bad_argument(const char* message, const char* argument) {
    print(message);
    print(argument);
    print(msg_quote_newline);
    return 2;
}

// A disk that is not mounted itself because it carries a partition table:
// point at its first partition instead
static void format_suggest_partition(struct BlockDevice* disk) {
    for (uint32_t i = 0; i < block_device_count(); i++) {
        struct BlockDevice* dev = block_get(i);
        if (block_partition_parent(dev) == disk) {
            print(disk->name);
            print(msg_use_partition);
            print(dev->name);
            print(" instead\n");
            return;
        }
    }
    print(disk->name);
    print(msg_not_mounted);
}

int console_cmd_format(int arg_count, char** args) {
    if (arg_count < 1 || args[0][0] == '-') {
        print(msg_usage);
        return 1;
    }

    // Device: "ata1" or "/mnt/ata1"
    const char* name = args[0];
    if (name[0] == '/' && name[1] == 'm' && name[2] == 'n' && name[3] == 't' && name[4] == '/')
        name += 5;

    struct BlockDevice* device = block_find(name);
    if (device == NULL) {
        print(msg_no_device);
        print(name);
        print("\n");
        return 1;
    }

    struct FSDeviceContext* active_ctx = vfs_device_context(device);
    if (active_ctx == NULL) {
        format_suggest_partition(device);
        return 1;
    }

    uint32_t max_capacity = fs_device_max_capacity(device);

    // Defaults: keep the current layout, or use the whole device when blank
    uint32_t total_capacity = max_capacity;
    uint32_t sector_size    = 512UL;
    bool     quick_format   = false;

    if (active_ctx->is_open) {
        struct FSPartitionBlock existing_part;
        vfs_lock();
        uint8_t has_part = fs_device_get_partition(active_ctx, &existing_part);
        vfs_unlock();

        if (has_part == 0) {
            if (existing_part.total_size > 0)  total_capacity = existing_part.total_size;
            if (existing_part.sector_size > 0) sector_size    = existing_part.sector_size;
        }
    }

    for (int i = 1; i < arg_count; i++) {
        char* argument = args[i];

        // Ensure argument starts with '-' and has at least a flag character
        if (argument[0] != '-' || argument[1] == '\0')
            return format_bad_argument(msg_unknown_arg, argument);

        char flag_type = argument[1];

        // Quick format request (e.g., "-q")
        if (flag_type == 'q') {
            if (argument[2] != '\0')
                return format_bad_argument(msg_unknown_arg, argument);
            quick_format = true;
        }
        // Handle flags that require numeric values (-k, -M, -s)
        else if (flag_type == 'k' || flag_type == 'M' || flag_type == 's') {
            if (argument[2] == '\0')
                return format_bad_argument(msg_missing_val, argument);

            // 64-bit so "-M99999" is caught as too large instead of wrapping
            uint64_t value = 0;
            const char* str = &argument[2];
            const char* startptr = str;

            while (*str >= '0' && *str <= '9') {
                value = value * 10 + (uint64_t)(*str - '0');
                if (value > 0xFFFFFFFFULL)
                    return format_bad_argument(msg_invalid_val, argument);
                str++;
            }

            if (str == startptr || *str != '\0')
                return format_bad_argument(msg_unknown_arg, argument);

            if (flag_type == 'k' || flag_type == 'M') {
                uint64_t bytes = value * ((flag_type == 'k') ? 1024ULL : 1024ULL * 1024ULL);
                if (bytes == 0)
                    return format_bad_argument(msg_invalid_val, argument);
                if (bytes > max_capacity) {
                    print(msg_too_large);
                    print_int(max_capacity);
                    print(msg_bytes_close);
                    return 3;
                }
                total_capacity = (uint32_t)bytes;
            }
            else if (value == 32 || value == 64 || value == 512 || value == 1024) {
                sector_size = (uint32_t)value;
            }
            else {
                return format_bad_argument(msg_invalid_val, argument);
            }
        }
        else {
            return format_bad_argument(msg_unknown_arg, argument);
        }
    }

    // A size that is not a whole number of blocks would leave a partial
    // block at the end that basefs can never allocate
    total_capacity -= total_capacity % sector_size;

    print(msg_formatting);
    print(device->name);
    print(msg_ellipsis);
    print_int(total_capacity);
    print(msg_bytes_total);
    print_int(sector_size);
    print(msg_sector_size);

    print(quick_format ? msg_quick_progress : msg_init_progress);

    // Wipe (unless quick), write the headers, bitmap and an empty root
    // directory, and re-open the mount, all under the VFS lock
    VfsFormatResult result = vfs_device_format_context(active_ctx, total_capacity, sector_size,
                                                       !quick_format, format_print_progress);

    if (result != VFS_FORMAT_OK) {
        display_cursor_set_position(0);
        if (result == VFS_FORMAT_BUSY) {
            print(msg_busy);
            return 5;
        }
        if (result == VFS_FORMAT_BAD_SIZE) {
            print(msg_too_large);
            print_int(max_capacity);
            print(msg_bytes_close);
            return 3;
        }
        print(msg_failed);
        return 4;
    }

    // The new root must read back. (The console's working directory needs
    // no fixing up: it is a path, and if it no longer exists the prompt
    // falls back to the nearest directory that does, here the device root.)
    struct FSPartitionBlock partition;
    vfs_lock();
    uint8_t has_part = fs_device_get_partition(active_ctx, &partition);
    vfs_unlock();

    if (has_part != 0 || partition.root_directory == FS_NULL) {
        display_cursor_set_position(0);
        print(msg_failed);
        return 4;
    }

    display_cursor_set_position(0);
    print(msg_final_progress);
    
    print("\n");
    return 0;
}

#endif

