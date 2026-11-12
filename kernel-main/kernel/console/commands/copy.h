#ifndef CONSOLE_CMD_COPY_H
#define CONSOLE_CMD_COPY_H

#include <stdint.h>
#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/util/string.h>
#include <kernel/console/commands/path_util.h>

// cp <path> <path or dir>
//
// Copies a file, or a directory with everything in it. When the destination
// is an existing directory, the copy goes inside it under the source's name.
// Built on vfs_copy(): it sizes the destination before writing, carries
// permissions over, refuses whole devices and copying a directory into
// itself, and leaves nothing behind when it fails.
static int cmd_copy_run(int arg_count, char** args) {
    if (arg_count < 2) 
        return 1;
    
    char src[VFS_PATH_MAX];
    char dest[VFS_PATH_MAX];
    if (!command_resolve_path(args[0], src, sizeof(src)) ||
        !command_resolve_path(args[1], dest, sizeof(dest)))
        return 2;
    
    if (!vfs_exists(src)) 
        return 3;   // Source does not exist
    if (strcmp(src, "/") == 0)
        return 4;   // Nothing to name the copy after
    
    if (vfs_directory_check(dest)) {
        // src is absolute and normalized: its name follows the last '/'
        const char* name = strrchr(src, '/') + 1;
        if (!vfs_path_normalize(dest, name, dest, sizeof(dest))) {
            print(msg_path_too_long);
            return 7;
        }
    }
    
    if (vfs_exists(dest))
        return 5;   // Destination already exists
    
    if (!vfs_copy(src, dest))
        return 6;
    
    return 0;
}

int console_cmd_copy(int arg_count, char** args) {
    int result = cmd_copy_run(arg_count, args);
    console_command_end();
    return result;
}

#endif
