#ifndef CONSOLE_CMD_CHKDSK_H
#define CONSOLE_CMD_CHKDSK_H

#include <stdint.h>
#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/util/string.h>
#include <kernel/console/commands/path_util.h>

// print_int() is signed: a 4 GB volume would print as negative
static void chkdsk_print_u32(uint32_t value) {
    char text[12];
    utos(value, text);
    print(text);
}

// chkdsk [path]   geometry of the device holding path (default: the
//                 working directory)
int console_cmd_chkdsk(int arg_count, char** args) {
    char path[VFS_PATH_MAX];
    if (!command_resolve_directory_arg(arg_count, args, path, sizeof(path)))
        return 1;
    
    VfsDeviceInfo info;
    if (!vfs_device_get_info(path, &info)) {
        print("Not on a mounted device\n");
        return 1;
    }
    
    print(info.device);
    print(": ");
    chkdsk_print_u32((uint32_t)(info.device_sectors * info.device_sector_size / (1024ULL * 1024ULL)));
    print(" MB, ");
    chkdsk_print_u32(info.device_sector_size);
    print("B sectors\n");
    
    if (!info.formatted || info.block_size == 0) {
        print("Not formatted\n\n");
        return 1;
    }
    
    chkdsk_print_u32(info.block_size);
    print("B per block\n");
    
    chkdsk_print_u32(info.total_size / info.block_size);
    print(" blocks\n");
    
    chkdsk_print_u32(info.total_size);
    print("B total\n\n");
    return 0;
}

#endif
