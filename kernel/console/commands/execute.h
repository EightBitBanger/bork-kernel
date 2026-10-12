#ifndef CONSOLE_CMD_EXECUTE_H
#define CONSOLE_CMD_EXECUTE_H

#include <stdint.h>
#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/console/commands/path_util.h>

// Run the program at args[0] with the rest as its arguments. Returns -1
// when there is no file there, so the console can try the next PATH entry.
// (Not a loader yet: prints what it would run.)
int console_cmd_execute(int arg_count, char** args) {
    if (arg_count < 1 || args == NULL || args[0] == NULL)
        return -1;
    
    char path[VFS_PATH_MAX];
    if (!command_resolve_path(args[0], path, sizeof(path)))
        return -1;
    
    // Directories and mounts are not programs
    FSFileStats stats;
    if (!vfs_stat(path, &stats) || stats.type != VFS_TYPE_FILE)
        return -1;
    
    print(path);
    print(" [");
    for (int i = 1; i < arg_count; i++) {
        print(args[i]);
        if (i + 1 < arg_count)
            print(", ");
    }
    print("]\n");
    
    return 0;
}

#endif
