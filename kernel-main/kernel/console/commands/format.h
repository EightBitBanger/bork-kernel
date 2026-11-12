#ifndef CONSOLE_CMD_FORMAT_H
#define CONSOLE_CMD_FORMAT_H

#include <stdint.h>
#include <stdbool.h>

#include <kernel/console/display.h>
#include <kernel/util/string.h>
#include <kernel/vfs/vfs.h>
#include <kernel/fs/format.h>

// Usage: format <device> [-q] [-t<filesystem>] [-k<KB> | -M<MB>] [-s<block size>]
//
//   <device>    block device to format: "ata1", "ahci0", "ata0p1" (or
//               "/mnt/ata1"). A whole disk is wiped from sector 0 to the
//               end, boot sector, partition table and every partition on
//               it included, and gets one whole-disk volume. A partition
//               ("ata1p1") is formatted inside its own range only.
//   -q          quick format: write the filesystem structures only. On a
//               whole disk the boot area, the partition table and the head
//               of every partition are still zeroed, so nothing on it can
//               be found or booted afterwards
//   -t          filesystem: "basefs" or "fat32" (also "fat"). Defaults to
//               the one the device holds now, or basefs on a whole disk.
//               fat32 is what other systems read: use it for flash drives
//   -k / -M     filesystem size; defaults to the current size, or the whole
//               device if it is unformatted
//   -s          allocation block size in bytes: 512, 1024, 2048 or 4096
//               (whatever the filesystem takes; basefs: a multiple of 4,
//               at least 512; fat32: a power of two up to 32768).
//               Defaults to the current one, else the filesystem's own
//               default (fat32 picks it by size, as Windows does).
//
// Shares its checks and wipe with SYS_FORMAT (kernel/fs/format.c). Here the
// system device may be formatted too.

static const char* msg_usage             = "Usage: format <device> [-q] [-t<filesystem>] [-k<KB> | -M<MB>] [-s<block size>]\n";
static const char* msg_no_device         = "No device named ";
static const char* msg_not_mounted       = " is not mounted\n";
static const char* msg_whole_disk        = "Erasing the whole drive: boot sector, partition table and all partitions\n";
static const char* msg_bytes_total       = " bytes total, ";
static const char* msg_sector_size       = " sector size\n\n";
static const char* msg_formatting        = "Formatting ";
static const char* msg_ellipsis          = "...\n";
static const char* msg_unknown_arg       = "Unknown argument '";
static const char* msg_missing_val       = "Missing value for argument '";
static const char* msg_invalid_val       = "Invalid value for argument '";
static const char* msg_too_large         = "Capacity exceeds the device (max ";
static const char* msg_bytes_close       = " bytes)\n";
static const char* msg_bad_block_size    = "Block size not supported: use 512, 1024, 2048 or 4096\n";
static const char* msg_quote_newline     = "'\n";
static const char* msg_percent           = "%";
static const char* msg_init_progress     = "0%";
static const char* msg_quick_progress    = "50%";
static const char* msg_final_progress    = "100%\n";

// print_int() is signed 32-bit: sizes past 2 GB need this
static void format_print_u64(uint64_t value) {
    char text[21];
    u64tos(value, text);
    print(text);
}

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

// Say why a format did not happen (or failed part way). Exit codes: 1 no
// such device, 3 bad size, 4 failed, 5 busy.
static int format_report(FormatResult result, const char* name, const FormatPlan* plan) {
    switch (result) {
        case FORMAT_OK:
            return 0;
        case FORMAT_NO_DEVICE:
            print(msg_no_device);
            print(name);
            print("\n");
            return 1;
        case FORMAT_NOT_MOUNTED:
            print(name);
            print(msg_not_mounted);
            return 1;
        case FORMAT_TOO_LARGE:
            print(msg_too_large);
            format_print_u64(plan->max_capacity);
            print(msg_bytes_close);
            return 3;
        case FORMAT_BAD_BLOCK_SIZE:
            print(msg_bad_block_size);
            return 3;
        case FORMAT_BUSY:
        case FORMAT_IN_PROGRESS:
            print(format_result_text(result));
            print("\n");
            return 5;
        default:
            print(format_result_text(result));
            print("\n");
            return (result == FORMAT_TOO_SMALL || result == FORMAT_BAD_FILESYSTEM) ? 3 : 4;
    }
}

static int cmd_format_run(int arg_count, char** args) {
    if (arg_count < 1 || args[0][0] == '-') {
        print(msg_usage);
        return 1;
    }

    // Device: "ata1" or "/mnt/ata1"
    const char* name = args[0];
    if (name[0] == '/' && name[1] == 'm' && name[2] == 'n' && name[3] == 't' && name[4] == '/')
        name += 5;

    // The kernel console may format any device, the system one included.
    // Unset sizes keep the current layout, or use the whole device.
    FormatRequest request;
    memset(&request, 0, sizeof(request));
    request.allow_system = true;

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
            request.quick = true;
        }
        // Filesystem (e.g., "-tfat32")
        else if (flag_type == 't') {
            if (argument[2] == '\0')
                return format_bad_argument(msg_missing_val, argument);
            request.filesystem = &argument[2];
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
            if (value == 0)
                return format_bad_argument(msg_invalid_val, argument);

            if (flag_type == 'k' || flag_type == 'M')
                request.capacity = value * ((flag_type == 'k') ? 1024ULL : 1024ULL * 1024ULL);
            else
                request.block_size = (uint32_t)value;   // The filesystem says which it takes
        }
        else {
            return format_bad_argument(msg_unknown_arg, argument);
        }
    }

    // Everything is checked before anything is written
    FormatPlan plan;
    FormatResult result = format_plan(name, &request, &plan);
    if (result != FORMAT_OK)
        return format_report(result, name, &plan);

    print(msg_formatting);
    print(plan.device);
    print(msg_ellipsis);
    format_print_u64(plan.capacity);
    print(msg_bytes_total);
    print(plan.filesystem);
    print("\n");
    print_int(plan.block_size);
    print(msg_sector_size);

    if (plan.whole_disk)
        print(msg_whole_disk);

    print(request.quick ? msg_quick_progress : msg_init_progress);

    // Plans again under the format lock and runs. (The console's working
    // directory needs no fixing up: it is a path, and if it no longer exists
    // the prompt falls back to the nearest directory that does, here the
    // device root.)
    result = format_run(name, &request, &plan, format_print_progress);
    display_cursor_set_position(0);
    if (result != FORMAT_OK)
        return format_report(result, name, &plan);

    print(msg_final_progress);
    return 0;
}

int console_cmd_format(int arg_count, char** args) {
    int result = cmd_format_run(arg_count, args);
    console_command_end();
    return result;
}

#endif

