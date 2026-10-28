#ifndef CONSOLE_CMD_ECHO_H
#define CONSOLE_CMD_ECHO_H

#include <stdint.h>
#include <kernel/kernel.h>
#include <kernel/console/commands/output.h>

// echo [text...]               print the words, separated by one space
// echo [text...] > <file>      write them as a line of the file (>> appends)
//
// Quote text to keep its spacing: echo "a   b" > f.txt
static int cmd_echo_run(int arg_count, char** args) {
    CommandOutput out;
    int word_count = command_output_parse(arg_count, args, &out);
    if (word_count < 0 || !command_output_open(&out))
        return 1;
    
    bool ok = true;
    for (int i = 0; i < word_count && ok; i++) {
        if (i > 0)
            ok = command_output_print(&out, " ");
        if (ok)
            ok = command_output_print(&out, args[i]);
    }
    if (ok)
        ok = command_output_print(&out, "\n");
    
    command_output_close(&out);
    if (!ok)
        print("Write failed\n");
    return ok ? 0 : 2;
}

int console_cmd_echo(int arg_count, char** args) {
    int result = cmd_echo_run(arg_count, args);
    console_command_end();
    return result;
}

#endif
