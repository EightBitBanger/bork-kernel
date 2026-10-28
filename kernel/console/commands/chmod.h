#ifndef CONSOLE_CMD_CHMOD_H
#define CONSOLE_CMD_CHMOD_H

#include <stdint.h>
#include <stdbool.h>
#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/console/commands/path_util.h>

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
// Only files and directories on a mounted storage device. Kernel nodes
// (/dev, /proc, ...) are refused here: the VFS would let the console flip
// them, but nothing should depend on that by accident.

static bool chmod_parse(const char* mode, char* op, uint8_t* bits) {
    *op = '=';
    *bits = 0;
    if (*mode == '+' || *mode == '-' || *mode == '=')
        *op = *mode++;

    for (; *mode != '\0'; mode++) {
        switch (*mode) {
            case 'x': *bits |= VFS_PERMISSION_EXECUTE; break;
            case 'r': *bits |= VFS_PERMISSION_READ;    break;
            case 'w': *bits |= VFS_PERMISSION_WRITE;   break;
            default:  return false;
        }
    }
    // "+" or "-" alone change nothing, which is almost certainly a typo
    return *op == '=' || *bits != 0;
}

static int cmd_chmod_run(int arg_count, char** args) {
    char op;
    uint8_t bits;
    if (arg_count < 2 || !chmod_parse(args[0], &op, &bits)) {
        print("chmod [+|-|=]<xrw> <path> [path...]\n");
        return 1;
    }

    int result = 0;
    for (int i = 1; i < arg_count; i++) {
        char path[VFS_PATH_MAX];
        if (!command_resolve_path(args[i], path, sizeof(path))) {
            result = 2;
            continue;
        }

        uint8_t permissions;
        if (!vfs_exists(path) || !vfs_get_permissions(path, &permissions)) {
            print(args[i]);
            print(": not found\n");
            result = 3;
            continue;
        }

        VfsDeviceInfo device;
        if (!vfs_device_get_info(path, &device)) {
            print(args[i]);
            print(": kernel node, not on a storage device\n");
            result = 4;
            continue;
        }
        
        if (op == '+')
            permissions |= bits;
        else if (op == '-')
            permissions &= (uint8_t)~bits;
        else
            permissions = bits;

        if (!vfs_set_permissions(path, permissions)) {
            print(args[i]);
            print(": cannot change permissions\n");
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
