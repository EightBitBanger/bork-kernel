#ifndef CONSOLE_CMD_LS_H
#define CONSOLE_CMD_LS_H

#include <stdint.h>
#include <kernel/kernel.h>
#include <kernel/console/console.h>
#include <kernel/console/commands/path_util.h>

int console_cmd_lsdir(int arg_count, char** args) {
    char path[VFS_PATH_MAX];
    if (!command_resolve_directory_arg(arg_count, args, path, sizeof(path)))
        return 1;
    
    if (!console_list_directory(path)) {
        print(msg_dir_invalid);
        return 1;
    }
    print("\n");
    return 0;
}

#endif
