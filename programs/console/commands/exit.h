#ifndef CONSOLE_CMD_EXIT_H
#define CONSOLE_CMD_EXIT_H

#include "console_host.h"

// exit         in a script: end it here. At the prompt: close the window.
int console_cmd_exit(int arg_count, char** args) {
    (void)arg_count;
    (void)args;

    console_command_end();
    console_exit();
    return 0;
}

#endif
