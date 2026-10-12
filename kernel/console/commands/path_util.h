#ifndef CONSOLE_CMD_PATH_UTIL_H
#define CONSOLE_CMD_PATH_UTIL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/console/print.h>
#include <kernel/console/console_const.h>

// Turn a command argument into the absolute, normalized path the VFS takes:
//
//   "/mnt/ata0/file"  -> "/mnt/ata0/file"
//   "file"            -> "<cwd>/file"
//   "../bin/./x"      -> "<parent of cwd>/bin/x"
//
// `out` should be VFS_PATH_MAX bytes. Prints a message and returns false
// if the result does not fit (it is never cut short, so a long path can
// never end up naming a different file).
static inline bool command_resolve_path(const char* arg, char* out, size_t out_size) {
    if (out_size > 0)
        out[0] = '\0';
    if (arg == NULL || arg[0] == '\0')
        return false;
    
    char cwd[VFS_PATH_MAX];
    kernel_get_working_directory(cwd, sizeof(cwd));
    
    if (!vfs_path_normalize(cwd, arg, out, out_size)) {
        print(msg_path_too_long);
        return false;
    }
    return true;
}

// The directory a command works on: its first argument if given, the
// working directory otherwise ("ls", "ls bin", "chkdsk /mnt/ata1")
static inline bool command_resolve_directory_arg(int arg_count, char** args, char* out, size_t out_size) {
    if (arg_count >= 1)
        return command_resolve_path(args[0], out, out_size);
    return command_resolve_path(".", out, out_size);
}

#endif
