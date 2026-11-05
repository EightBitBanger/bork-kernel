#ifndef CONSOLE_CMD_MOVE_H
#define CONSOLE_CMD_MOVE_H

#include <stdint.h>
#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/util/string.h>
#include <kernel/console/commands/path_util.h>

// mv <path> <path or dir>
//
// Moves a file, or a directory with everything in it. When the destination
// is an existing directory, the item goes inside it under its own name.
// On one volume vfs_move() relinks the directory entry and nothing is
// copied. Across volumes it is vfs_copy() and then vfs_remove() of the
// original; if that fails, both are left rather than losing one.
static int cmd_move_run(int arg_count, char** args) {
    if (arg_count < 2)
        return 1;

    char src[VFS_PATH_MAX];
    char dest[VFS_PATH_MAX];
    if (!command_resolve_path(args[0], src, sizeof(src)) ||
        !command_resolve_path(args[1], dest, sizeof(dest)))
        return 2;

    if (!vfs_exists(src))
        return 3;   // Source does not exist
    if (strcmp(src, "/") == 0 || vfs_directory_check_mounted(src))
        return 4;   // Mounts are named after their device

    if (vfs_directory_check(dest)) {
        // src is absolute and normalized: its name follows the last '/'
        const char* name = strrchr(src, '/') + 1;
        if (!vfs_path_normalize(dest, name, dest, sizeof(dest))) {
            print(msg_path_too_long);
            return 7;
        }
    }
    if (strcmp(src, dest) == 0)
        return 0;

    if (vfs_exists(dest))
        return 5;   // Destination already exists

    VfsMoveResult moved = vfs_move(src, dest);
    if (moved == VFS_MOVE_OK)
        return 0;
    if (moved != VFS_MOVE_CROSS_DEVICE)
        return 6;

    if (!vfs_copy(src, dest))
        return 8;
    if (!vfs_remove(src))
        return 9;   // Both are left
    return 0;
}

int console_cmd_move(int arg_count, char** args) {
    int result = cmd_move_run(arg_count, args);
    console_command_end();
    return result;
}

#endif
