#ifndef CONSOLE_CMD_CD_H
#define CONSOLE_CMD_CD_H

#include <stdint.h>
#include <stdbool.h>

#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/util/string.h>
#include <kernel/console/console.h>
#include <kernel/console/console_const.h>
#include <kernel/console/commands/path_util.h>

// cd <dir>     change the working directory (absolute or relative, ".." ok)
// cd, cd .     print the working directory
//
// The working directory is a path (see kernel.h), so this only has to
// normalize the argument and check that a directory is there. Everything
// else (entering and leaving devices, ".." out of a device root) is the
// VFS's path resolution, the same rules every other command gets.
static int cmd_chdir_run(int arg_count, char** args) {
    if (arg_count < 1 || strcmp(args[0], ".") == 0) {
        char cwd[VFS_PATH_MAX];
        kernel_get_working_directory(cwd, sizeof(cwd));
        print(cwd);
        print("\n");
        return 0;
    }
    
    char target[VFS_PATH_MAX];
    if (!command_resolve_path(args[0], target, sizeof(target)))
        return 1;
    
    if (!vfs_directory_check(target)) {
        print(msg_dir_invalid);
        return 1;
    }
    
    kernel_set_working_directory(target);
    console_prompt_refresh();
    return 0;
}

int console_cmd_chdir(int arg_count, char** args) {
    int result = cmd_chdir_run(arg_count, args);
    console_command_end();
    return result;
}

#endif
