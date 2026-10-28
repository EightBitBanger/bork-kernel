#ifndef CONSOLE_CMD_MKDIR_H
#define CONSOLE_CMD_MKDIR_H

#include <stdint.h>
#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/console/commands/path_util.h>

// mkdir <dir>
static int cmd_mkdir_run(int arg_count, char** args) {
    if (arg_count == 0) 
        return 1;
    
    char path[VFS_PATH_MAX];
    if (!command_resolve_path(args[0], path, sizeof(path)))
        return 2;
    
    if (!vfs_mkdir(path)) 
        return 4;
    
    return 0;
}

int console_cmd_mkdir(int arg_count, char** args) {
    int result = cmd_mkdir_run(arg_count, args);
    console_command_end();
    return result;
}

#endif
