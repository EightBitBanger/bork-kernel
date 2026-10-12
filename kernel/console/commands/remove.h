#ifndef CONSOLE_CMD_RM_H
#define CONSOLE_CMD_RM_H

#include <stdint.h>
#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/console/commands/path_util.h>

// rm <path>   remove a file, or a directory with everything in it. Removing
// the working directory (or one above it) is fine: the prompt falls back
// to the nearest directory that still exists.
int console_cmd_rm(int arg_count, char** args) {
    if (arg_count == 0) 
        return 1;
    
    char path[VFS_PATH_MAX];
    if (!command_resolve_path(args[0], path, sizeof(path)))
        return 2;
    
    if (!vfs_exists(path)) 
        return 4;
    
    if (!vfs_remove(path)) 
        return 5;
    
    return 0;
}

#endif
