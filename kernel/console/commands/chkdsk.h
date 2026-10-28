#ifndef CONSOLE_CMD_CHKDSK_H
#define CONSOLE_CMD_CHKDSK_H

#include <stdint.h>
#include <stdbool.h>
#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/block/block.h>
#include <kernel/block/partition.h>
#include <kernel/util/string.h>

// chkdsk <device> [-f]
//
//   chkdsk ahci0p1      check that partition
//   chkdsk ahci0        check every partition on the drive (or its
//                       whole-disk volume)
//   chkdsk ahci0p1 -f   fix what is found
//
// Walks the volume's folder tree: damaged folder entries, entries that point
// at nothing or at something linked elsewhere, broken file data chains,
// sizes that do not match the data, wrong parent links, and blocks that are
// allocated but used by nothing (or used but marked free). Without -f
// nothing is written to the drive. Repairing needs every file on the volume
// closed.

static const char* msg_chkdsk_usage = "Usage: chkdsk <device> [-f]   (-f fixes what is found)\n";

// print_int() is signed: a 4 GB volume would print as negative
static void chkdsk_print_u32(uint32_t value) {
    char text[12];
    utos(value, text);
    print(text);
}

static void chkdsk_print_u64(uint64_t value) {
    char text[21];
    u64tos(value, text);
    print(text);
}

static void chkdsk_print_count(uint64_t count, const char* one, const char* many) {
    chkdsk_print_u64(count);
    print(count == 1 ? one : many);
}

// vfs_check_problem_fn: one line per problem, under the device's /mnt path
static void chkdsk_problem(void* user, const char* path, const char* problem, bool fixed) {
    const char* device = (const char*)user;
    print("  ");
    if (path[0] != '\0') {
        print("/mnt/");
        print(device);
        if (strcmp(path, "/") != 0)
            print(path);
        print(": ");
    }
    print(problem);
    print(fixed ? " (fixed)\n" : "\n");
}

// Check one volume. 0 when it is sound (or was made so), 2 when it could
// not be checked, 3 when problems remain.
static int chkdsk_volume(struct BlockDevice* device, bool repair) {
    char path[8 + BLOCK_NAME_MAX];
    strcpy(path, "/mnt/");
    strcat(path, device->name);

    print(device->name);
    print(": ");

    struct FsMount* mount = vfs_device_context(device);
    VfsDeviceInfo info;
    if (mount == NULL || !vfs_device_get_info(path, &info)) {
        print("not mounted\n");
        return 2;
    }
    if (!info.formatted || info.block_size == 0) {
        print("not formatted\n");
        return 2;
    }

    print(info.filesystem);
    print(", ");
    chkdsk_print_u64(info.total_size / (1024ULL * 1024ULL));
    print(" MB, ");
    chkdsk_print_u32(info.block_size);
    print("B blocks\n");

    VfsCheckReport report;
    report.problem = chkdsk_problem;
    report.user    = device->name;
    VfsCheckResult result = vfs_device_check(mount, repair, &report);

    switch (result) {
        case VFS_CHECK_OK:
            break;
        case VFS_CHECK_BUSY:
            print("  Files on ");
            print(device->name);
            print(" are open, close them first\n");
            return 2;
        case VFS_CHECK_UNSUPPORTED:
            print("  No checker for ");
            print(info.filesystem);
            print("\n");
            return 2;
        case VFS_CHECK_NO_MEMORY:
            print("  Out of memory, check stopped\n");
            return 2;
        case VFS_CHECK_IO_ERROR:
            print("  Writing the repairs failed\n");
            return 2;
        default:
            print("  Not mounted\n");
            return 2;
    }

    // What is on it
    print("  ");
    chkdsk_print_count(report.directories, " folder, ", " folders, ");
    chkdsk_print_count(report.files, " file, ", " files, ");
    chkdsk_print_u64((report.file_bytes + 1023) / 1024);
    print(" KB of data");
    if (report.blocks_checked) {
        print(", ");
        chkdsk_print_u64(report.blocks_used);
        print(" of ");
        chkdsk_print_u64(report.blocks_total);
        print(" blocks used");
    }
    print("\n");
    if (!report.blocks_checked && report.problems == 0)
        print("  Too large to check the allocation table\n");

    if (report.problems == 0) {
        print("  No problems found\n");
        return 0;
    }

    print("  ");
    chkdsk_print_count(report.problems, " problem", " problems");
    if (!repair) {
        print(" found, chkdsk ");
        print(device->name);
        print(" -f fixes them\n");
        return 3;
    }
    if (report.fixed == report.problems) {
        print(" fixed\n");
        return 0;
    }
    print(", ");
    chkdsk_print_u32(report.fixed);
    print(" fixed\n");
    return 3;
}

static int cmd_chkdsk_run(int arg_count, char** args) {
    const char* name = NULL;
    bool repair = false;
    for (int i = 0; i < arg_count; i++) {
        if (strcmp(args[i], "-f") == 0) {
            repair = true;
        } else if (args[i][0] == '-' || name != NULL) {
            print(msg_chkdsk_usage);
            return 1;
        } else {
            name = args[i];
        }
    }
    if (name == NULL) {
        print(msg_chkdsk_usage);
        return 1;
    }

    // Device: "ahci0p1" or "/mnt/ahci0p1"
    if (strncmp(name, "/mnt/", 5) == 0)
        name += 5;

    struct BlockDevice* device = block_find(name);
    if (device == NULL) {
        print("No device named ");
        print(name);
        print("\n");
        return 2;
    }

    // A whole drive: each partition on it, in order
    int result = 0;
    bool any = false;
    if (block_partition_parent(device) == NULL) {
        for (uint32_t i = 0; i < block_device_count(); i++) {
            struct BlockDevice* part = block_get(i);
            if (part == NULL || block_partition_parent(part) != device)
                continue;
            if (any)
                print("\n");
            int volume = chkdsk_volume(part, repair);
            if (volume > result)
                result = volume;
            any = true;
        }
    }

    // A partition, or a drive holding one volume over all of it
    if (!any)
        result = chkdsk_volume(device, repair);
    return result;
}

int console_cmd_chkdsk(int arg_count, char** args) {
    int result = cmd_chkdsk_run(arg_count, args);
    console_command_end();
    return result;
}

#endif
