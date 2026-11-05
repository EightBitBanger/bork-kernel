#ifndef CONSOLE_CMD_FORMAT_H
#define CONSOLE_CMD_FORMAT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <os/disk.h>
#include <os/security.h>
#include "console_host.h"

// format <device> [-q] [-t<filesystem>] [-k<KB> | -M<MB>] [-s<block size>]
//
// The same arguments as the kernel console's format, done through
// disk_format() (os/disk.h). The differences come from running as a
// program: formatting asks the user for admin first (the desktop's
// prompt), the device holding the system folders is refused (use the
// kernel console for that one), and there is no progress count, only
// the starting figure (0%, or 50% for -q) and 100%, each on its own line.

static const char* msg_usage             = "Usage: format <device> [-q] [-t<filesystem>] [-k<KB> | -M<MB>] [-s<block size>]\n";
static const char* msg_no_device         = "No device named ";
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
static const char* msg_init_progress     = "0%\n";
static const char* msg_quick_progress    = "50%\n";
static const char* msg_final_progress    = "100%\n";

static void format_print_u64(uint64_t value) {
    char text[21];
    snprintf(text, sizeof(text), "%llu", (unsigned long long)value);
    print(text);
}

static int format_bad_argument(const char* message, const char* argument) {
    print(message);
    print(argument);
    print(msg_quote_newline);
    return 2;
}

// Say why a format did not happen (or failed part way), from disk_format's
// errno. Exit codes as in the kernel: 1 no such device, 3 bad size,
// 4 failed, 5 busy, 6 refused.
static int format_report(int err, const char* name, const disk_format_t* spec) {
    switch (err) {
        case ENOENT:
            print(msg_no_device);
            print(name);
            print(" (or it is not mounted)\n");
            return 1;
        case ENOSPC:
            print(msg_too_large);
            format_print_u64(spec->max_capacity);
            print(msg_bytes_close);
            return 3;
        case EINVAL:
            if (spec->block_size != 0 &&
                (spec->block_size < DISK_BLOCK_MIN || spec->block_size > DISK_BLOCK_MAX ||
                 (spec->block_size & (spec->block_size - 1)) != 0))
                print(msg_bad_block_size);
            else
                print("The device can't take that filesystem or size\n");
            return 3;
        case EACCES:
            print(name);
            print(" holds the system folders: format it from the kernel console\n");
            return 6;
        case EBUSY:
            print("Files on the device are open, or another format is running\n");
            return 5;
        case EPERM:
            print("No clearance to format devices\n");
            return 6;
        case EIO:
            print("The device failed\n");
            return 4;
        default:
            print(strerror(err));
            print("\n");
            return 4;
    }
}

static int cmd_format_run(int arg_count, char** args) {
    if (arg_count < 1 || args[0][0] == '-') {
        print(msg_usage);
        return 1;
    }

    // Device: "ata1" or "/mnt/ata1"
    const char* name = args[0];
    if (strncmp(name, "/mnt/", 5) == 0)
        name += 5;

    // Unset sizes keep the current layout, or use the whole device
    disk_format_t spec;
    disk_format_init(&spec);

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
            spec.flags |= DISK_FORMAT_QUICK;
        }
        // Filesystem (e.g., "-tfat32")
        else if (flag_type == 't') {
            if (argument[2] == '\0')
                return format_bad_argument(msg_missing_val, argument);
            if (strlen(&argument[2]) >= sizeof(spec.filesystem))
                return format_bad_argument(msg_invalid_val, argument);
            strcpy(spec.filesystem, &argument[2]);
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
                spec.capacity = value * ((flag_type == 'k') ? 1024ULL : 1024ULL * 1024ULL);
            else
                spec.block_size = (uint32_t)value;   // The filesystem says which it takes
        }
        else {
            return format_bad_argument(msg_unknown_arg, argument);
        }
    }

    // Everything is checked before anything is written. The query fills
    // the spec in, so the request is kept for the real run.
    disk_format_t request = spec;
    if (disk_format_query(name, &spec) < 0) {
        int err = errno;
        if (err == EINVAL)
            spec.block_size = request.block_size;   // So the report can tell why
        return format_report(err, name, &spec);
    }

    print(msg_formatting);
    print(name);
    print(msg_ellipsis);
    format_print_u64(spec.capacity);
    print(msg_bytes_total);
    print(spec.filesystem);
    print("\n");
    char text[12];
    snprintf(text, sizeof(text), "%u", (unsigned)spec.block_size);
    print(text);
    print(msg_sector_size);

    if (spec.info & DISK_INFO_WHOLE_DISK)
        print(msg_whole_disk);

    // Admin only for as long as the format takes, and only if we asked
    bool had_admin = (sec_get() & SEC_ADMIN) != 0;
    if (!had_admin) {
        char reason[96];
        snprintf(reason, sizeof(reason), "Erase %s and format it as %s", name, spec.filesystem);
        console_flush();                            // The plan shows while the prompt is up
        if (sec_elevate(reason) < 0) {
            print(errno == EACCES ? "Cancelled\n" : "Can't ask for administrator rights\n");
            return 6;
        }
    }

    print((request.flags & DISK_FORMAT_QUICK) ? msg_quick_progress : msg_init_progress);
    console_flush();

    // Blocks until it is done. (If the console's working directory was on
    // the device, the host falls back to the nearest one that exists.)
    int r = disk_format(name, &request);
    int err = errno;
    if (!had_admin)
        sec_drop(SEC_ADMIN);
    console_cwd_changed();
    if (r < 0) {
        print("\n");
        return format_report(err, name, &request);
    }

    print(msg_final_progress);
    return 0;
}

int console_cmd_format(int arg_count, char** args) {
    int result = cmd_format_run(arg_count, args);
    console_command_end();
    return result;
}

#endif
