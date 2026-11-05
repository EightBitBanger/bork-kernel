#ifndef CONSOLE_CMD_PATH_UTIL_H
#define CONSOLE_CMD_PATH_UTIL_H

#include <stddef.h>
#include <stdbool.h>
#include <limits.h>
#include <sys/stat.h>
#include <os/os.h>

#include "console_host.h"
#include "console_const.h"

#define CMD_PATH_MAX  PATH_MAX

// Turn a command argument into an absolute, normalized path:
//
//   "/mnt/ata0/file"  -> "/mnt/ata0/file"
//   "file"            -> "<cwd>/file"
//   "../bin/./x"      -> "<parent of cwd>/bin/x"
//
// `out` should be CMD_PATH_MAX bytes. Prints a message and returns false
// if the result does not fit (it is never cut short, so a long path can
// never end up naming a different file).
static inline bool command_resolve_path(const char* arg, char* out, size_t out_size) {
    if (out_size > 0)
        out[0] = '\0';
    if (arg == NULL || arg[0] == '\0')
        return false;

    if (os_abspath(arg, out, out_size) == NULL) {
        print(msg_path_too_long);
        return false;
    }
    return true;
}

// The directory a command works on: its first argument if given, the
// working directory otherwise ("ls", "ls bin")
static inline bool command_resolve_directory_arg(int arg_count, char** args, char* out, size_t out_size) {
    if (arg_count >= 1)
        return command_resolve_path(args[0], out, out_size);
    return command_resolve_path(".", out, out_size);
}

static inline bool command_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static inline bool command_is_directory(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

#endif
