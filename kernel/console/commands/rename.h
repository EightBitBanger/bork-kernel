#ifndef CONSOLE_CMD_RN_H
#define CONSOLE_CMD_RN_H

#include <stdint.h>
#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/console/commands/path_util.h>

// rn <path> <new name>   the new name is a name, not a path (no '/')
int console_cmd_rename(int arg_count, char** args) {
    if (arg_count < 2) 
        return 1;
    
    char path[VFS_PATH_MAX];
    if (!command_resolve_path(args[0], path, sizeof(path)))
        return 3;
    
    if (!vfs_exists(path)) 
        return 4;
    
    if (!vfs_rename(path, args[1])) 
        return 2;
    
    return 0;
}

#endif
