#ifndef CONSOLE_CMD_KILL_H
#define CONSOLE_CMD_KILL_H

#include <stdint.h>
#include <kernel/kernel.h>
#include <kernel/util/string.h>
#include <kernel/scheduler/process.h>

// kill <pid> [pid...]   end processes (pids as ps lists them)
//
// The program started from the prompt is ended with Ctrl+C instead; this is
// for the ones running in the background, such as the window manager.
static int cmd_kill_run(int arg_count, char** args) {
    if (arg_count == 0) {
        print("kill <pid> [pid...]\n");
        return 1;
    }
    
    int result = 0;
    for (int i = 0; i < arg_count; i++) {
        const char* text = args[i];
        bool digits = (text[0] != '\0');
        for (const char* c = text; *c != '\0'; c++)
            if (*c < '0' || *c > '9')
                digits = false;
        
        ProcessId pid = digits ? (ProcessId)stou(text) : PROCESS_INVALID_ID;
        if (pid <= PROCESS_INVALID_ID) {
            print(text);
            print(": not a process id\n");
            result = 2;
            continue;
        }
        
        int code = DestroyProcess(pid);
        if (code != PROCESS_OK) {
            print(text);
            print(": ");
            print(process_error_string(code));
            print("\n");
            result = 3;
        }
    }
    
    return result;
}

int console_cmd_kill(int arg_count, char** args) {
    int result = cmd_kill_run(arg_count, args);
    console_command_end();
    return result;
}

#endif
