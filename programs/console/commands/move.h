#ifndef CONSOLE_CMD_MOVE_H
#define CONSOLE_CMD_MOVE_H

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <os/os.h>
#include "console_host.h"
#include "console_const.h"
#include "path_util.h"
#include "remove.h"         // rm_tree

// mv <path> <path or dir>
//
// Moves a file, or a directory with everything in it. When the destination
// is an existing directory, the item goes inside it under its own name.
//
// On one volume this is os_move(): the kernel relinks the directory entry
// and nothing is copied, so a 1 GB file moves as fast as an empty one. On
// another volume (EXDEV) it is os_copy() and then the original is removed;
// if the original can't be removed, both are left rather than losing one.
static int cmd_move_run(int arg_count, char** args) {
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
        return 4;

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
    if (strcmp(src, dest) == 0)
        return 0;   // Already there

    if (command_exists(dest)) {
        print("Destination already exists\n");
        return 5;
    }

    if (os_move(src, dest) == 0)
        return 0;
    if (errno == EINVAL) {
        print("A folder can't be moved into itself\n");
        return 6;
    }
    if (errno != EXDEV && errno != ENOSYS) {
        print("Could not move it\n");
        return 6;
    }

    // Another volume: copy, then remove the original
    if (os_copy(src, dest) < 0) {
        print("Could not copy it to the other volume\n");
        return 8;
    }
    if (!rm_tree(src, 0)) {
        print("Copied, but the original could not be removed\n");
        return 9;
    }
    return 0;
}

int console_cmd_move(int arg_count, char** args) {
    int result = cmd_move_run(arg_count, args);
    console_cwd_changed();      // cd'd into what was moved: fall back
    console_command_end();
    return result;
}

#endif
