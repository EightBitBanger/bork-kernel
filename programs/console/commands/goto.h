#ifndef CONSOLE_CMD_GOTO_H
#define CONSOLE_CMD_GOTO_H

#include "console_host.h"

// goto <label>     in a script: carry on from the line after ":label"
// goto :eof        in a script: end it here (like exit)
//
// As in a Windows batch file: a label is a line starting with ':', the
// name is matched without regard to case, and the ':' may be given or
// left out ("goto top" and "goto :top" both find ":top"). A label that
// isn't in the script ends it, as a failed line. Only one label line per
// name counts: the first one, from the top of the file.
int console_cmd_goto(int arg_count, char** args) {
    if (arg_count != 1) {
        print("Usage: goto <label>\n");
        console_command_end();
        return 1;
    }
    return console_goto(args[0]) == 0 ? 0 : 1;
}

#endif
