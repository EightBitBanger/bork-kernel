#ifndef CONSOLE_CMD_CLEAR_H
#define CONSOLE_CMD_CLEAR_H

#include "console_host.h"

int console_cmd_clear(int arg_count, char** args) {
    (void)arg_count;
    (void)args;

    console_clear();
    return 0;
}

#endif
