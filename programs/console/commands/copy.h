#ifndef CONSOLE_CMD_COPY_H
#define CONSOLE_CMD_COPY_H

#include <stdio.h>
#include <string.h>
#include <os/os.h>
#include "console_host.h"
#include "console_const.h"
#include "path_util.h"

// cp <path> <path or dir>
//
// Copies a file, or a directory with everything in it. When the destination
// is an existing directory, the copy goes inside it under the source's name.
// Built on os_copy() (the kernel's vfs_copy): it sizes the destination
// before writing, carries permissions over, refuses whole devices and
// copying a directory into itself, and leaves nothing behind when it fails.
static int cmd_copy_run(int arg_count, char** args) {
    if (arg_count < 2)
        return 1;

    char src[CMD_PATH_MAX];
    char dest[CMD_PATH_MAX];
    if (!command_resolve_path(args[0], src, sizeof(src)) ||
        !command_resolve_path(args[1], dest, sizeof(dest)))
        return 2;

    if (!command_exists(src))
        return 3;   // Source does not exist
    if (strcmp(src, "/") == 0)
        return 4;   // Nothing to name the copy after

    if (command_is_directory(dest)) {
        // src is absolute and normalized: its name follows the last '/'
        const char* name = strrchr(src, '/') + 1;
        char inside[CMD_PATH_MAX];
        int n = snprintf(inside, sizeof(inside), "%s%s%s", dest, strcmp(dest, "/") == 0 ? "" : "/", name);
        if (n < 0 || (size_t)n >= sizeof(inside)) {
            print(msg_path_too_long);
            return 7;
        }
        strcpy(dest, inside);
    }

    if (command_exists(dest))
        return 5;   // Destination already exists

    if (os_copy(src, dest) < 0)
        return 6;

    return 0;
}

int console_cmd_copy(int arg_count, char** args) {
    int result = cmd_copy_run(arg_count, args);
    console_command_end();
    return result;
}

#endif
