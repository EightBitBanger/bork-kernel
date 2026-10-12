#ifndef CONSOLE_CMD_HELP_H
#define CONSOLE_CMD_HELP_H

#include <stdint.h>
#include <kernel/console/command.h>
#include <kernel/console/print.h>
#include <kernel/util/string.h>

#define CONSOLE_HELP_NAME_WIDTH  10

// help   list the built-in commands
int console_cmd_help(int arg_count, char** args) {
    (void)arg_count;
    (void)args;

    for (uint32_t i = 0; i < console_command_count(); i++) {
        const ConsoleCommand* cmd = console_command_get(i);
        if (cmd == NULL || !(cmd->flags & CONSOLE_CMD_FLAG_BUILTIN))
            continue;

        print(cmd->name);
        if (cmd->help != NULL) {
            for (size_t pad = strlen(cmd->name); pad < CONSOLE_HELP_NAME_WIDTH; pad++)
                print(" ");
            print(cmd->help);
        }
        print("\n");
    }
    return 0;
}

#endif
