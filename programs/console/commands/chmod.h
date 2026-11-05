#ifndef CONSOLE_CMD_CHMOD_H
#define CONSOLE_CMD_CHMOD_H

#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/stat.h>
#include <os/os.h>
#include "console_host.h"
#include "path_util.h"

// chmod <mode> <path> [path...]
//
// Changes the x r w permissions that ls shows. <mode> is a set of the
// letters x, r and w, with an optional operator in front:
//
//   chmod +x prog       add execute
//   chmod -w notes.txt  remove write
//   chmod rw notes.txt  exactly read and write (same as =rw)
//   chmod = secret      no permissions at all
//
// The kernel decides what may be changed (os_set_permissions): kernel
// nodes and device roots are refused there.

static bool chmod_parse(const char* mode, char* op, uint32_t* bits) {
    *op = '=';
    *bits = 0;
    if (*mode == '+' || *mode == '-' || *mode == '=')
        *op = *mode++;

    for (; *mode != '\0'; mode++) {
        switch (*mode) {
            case 'x': *bits |= SYS_PERM_EXECUTE; break;
            case 'r': *bits |= SYS_PERM_READ;    break;
            case 'w': *bits |= SYS_PERM_WRITE;   break;
            default:  return false;
        }
    }
    // "+" or "-" alone change nothing, which is almost certainly a typo
    return *op == '=' || *bits != 0;
}

static int cmd_chmod_run(int arg_count, char** args) {
    char op;
    uint32_t bits;
    if (arg_count < 2 || !chmod_parse(args[0], &op, &bits)) {
        print("chmod [+|-|=]<xrw> <path> [path...]\n");
        return 1;
    }

    int result = 0;
    for (int i = 1; i < arg_count; i++) {
        char path[CMD_PATH_MAX];
        if (!command_resolve_path(args[i], path, sizeof(path))) {
            result = 2;
            continue;
        }

        struct stat st;
        if (stat(path, &st) < 0) {
            print(args[i]);
            print(": not found\n");
            result = 3;
            continue;
        }

        uint32_t permissions = 0;
        if (st.st_mode & S_IXUSR) permissions |= SYS_PERM_EXECUTE;
        if (st.st_mode & S_IRUSR) permissions |= SYS_PERM_READ;
        if (st.st_mode & S_IWUSR) permissions |= SYS_PERM_WRITE;

        if (op == '+')
            permissions |= bits;
        else if (op == '-')
            permissions &= ~bits;
        else
            permissions = bits;

        if (os_set_permissions(path, permissions) < 0) {
            print(args[i]);
            print(errno == EBUSY ? ": a device's root, cannot change permissions\n"
                                 : ": cannot change permissions\n");
            result = 5;
        }
    }

    return result;
}

int console_cmd_chmod(int arg_count, char** args) {
    int result = cmd_chmod_run(arg_count, args);
    console_command_end();
    return result;
}

#endif
