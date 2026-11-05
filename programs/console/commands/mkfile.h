#ifndef CONSOLE_CMD_MK_H
#define CONSOLE_CMD_MK_H

#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include "path_util.h"

// mk <file> [size]   create a file (zero-filled to size bytes)
static int cmd_mk_run(int arg_count, char** args) {
    if (arg_count == 0)
        return 1;

    char path[CMD_PATH_MAX];
    if (!command_resolve_path(args[0], path, sizeof(path)))
        return 2;

    int file = open(path, O_WRONLY | O_CREAT);
    if (file < 0)
        return 4;
    close(file);

    if (arg_count >= 2 && truncate(path, (off_t)(uint32_t)atoi(args[1])) < 0)
        return 5;   // No room on the device

    return 0;
}

int console_cmd_mk(int arg_count, char** args) {
    int result = cmd_mk_run(arg_count, args);
    console_command_end();
    return result;
}

#endif
