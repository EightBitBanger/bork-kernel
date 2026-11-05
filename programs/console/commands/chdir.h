#ifndef CONSOLE_CMD_CD_H
#define CONSOLE_CMD_CD_H

#include <string.h>
#include <unistd.h>
#include "console_host.h"
#include "console_const.h"
#include "path_util.h"

// cd <dir>     change the working directory (absolute or relative, ".." ok)
// cd, cd .     print the working directory
//
// The program's own working directory: programs started from here start
// in it, as from the kernel console.
static int cmd_chdir_run(int arg_count, char** args) {
    if (arg_count < 1 || strcmp(args[0], ".") == 0) {
        char cwd[CMD_PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) == NULL)
            strcpy(cwd, "/");
        print(cwd);
        print("\n");
        return 0;
    }

    char target[CMD_PATH_MAX];
    if (!command_resolve_path(args[0], target, sizeof(target)))
        return 1;

    if (!command_is_directory(target) || chdir(target) < 0) {
        print(msg_dir_invalid);
        return 1;
    }

    console_cwd_changed();
    return 0;
}

int console_cmd_chdir(int arg_count, char** args) {
    int result = cmd_chdir_run(arg_count, args);
    console_command_end();
    return result;
}

#endif
