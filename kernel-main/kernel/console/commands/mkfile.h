#ifndef CONSOLE_CMD_MK_H
#define CONSOLE_CMD_MK_H

#include <stdint.h>
#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/console/commands/path_util.h>

// mk <file> [size]   create a file (zero-filled to size bytes)
static int cmd_mk_run(int arg_count, char** args) {
    if (arg_count == 0) 
        return 1;
    
    char path[VFS_PATH_MAX];
    if (!command_resolve_path(args[0], path, sizeof(path)))
        return 2;
    
    File file = vfs_open(path, VFS_OPEN_CREATE | VFS_OPEN_WRITE);
    if (file == VFS_INVALID_FILE) 
        return 4;
    vfs_close(file);
    
    if (arg_count >= 2 && !vfs_truncate(path, (uint32_t)stoi(args[1])))
        return 5;   // No room on the device
    
    return 0;
}

int console_cmd_mk(int arg_count, char** args) {
    int result = cmd_mk_run(arg_count, args);
    console_command_end();
    return result;
}

#endif
