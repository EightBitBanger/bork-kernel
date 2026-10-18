#ifndef _KERNEL_CONSOLE_COMMAND_H_
#define _KERNEL_CONSOLE_COMMAND_H_

#include <stdint.h>

// Console command registry
//
// The built-in shell commands (ls, cd, format, ...) and the "run a program"
// step the console falls back to. This used to live in syscall/ under
// syscall_* names; syscall/ now holds only the ring 3 system call interface.
//
// Each command is a console_cmd_* function in console/commands/<name>.h,
// called with the words typed after the command name:
//
//   "cp a.txt /mnt/ata0"  ->  console_cmd_copy(2, { "a.txt", "/mnt/ata0" })
//
// Return 0 on success, anything else on failure.
//
// Adding a command: write console/commands/<name>.h, include it in
// console/command.c, add an id below (before CONSOLE_CMD_COUNT) and a row
// to the table there.

typedef int (*ConsoleCommandFn)(int arg_count, char** args);

// CONSOLE_CMD_EXECUTE's result when there is no program at that path: the
// console moves on to the next PATH entry. Every other result means the
// program was found (it ran, or the reason it could not was printed), so
// the search stops there. Also what console_command_run() returns for an
// unknown id.
#define CONSOLE_CMD_NOT_FOUND   (-1)

typedef enum {
    CONSOLE_CMD_EXECUTE,    // Run a program by path (internal, no name)
    CONSOLE_CMD_HELP,
    CONSOLE_CMD_CLS,
    CONSOLE_CMD_LIST,
    CONSOLE_CMD_CHDIR,
    CONSOLE_CMD_MAKE,
    CONSOLE_CMD_MAKEDIR,
    CONSOLE_CMD_REMOVE,
    CONSOLE_CMD_RENAME,
    CONSOLE_CMD_COPY,
    CONSOLE_CMD_TYPE,
    CONSOLE_CMD_FORMAT,
    CONSOLE_CMD_CHKDSK,
    CONSOLE_CMD_BOOT,

    CONSOLE_CMD_COUNT
} ConsoleCommandId;

// Typed at the prompt by name (and listed by "help")
#define CONSOLE_CMD_FLAG_BUILTIN   0x01

typedef struct {
    const char*      name;      // What the user types ("" for internal entries)
    ConsoleCommandId id;
    ConsoleCommandFn function;
    uint8_t          flags;     // CONSOLE_CMD_FLAG_*
    const char*      help;      // One line for "help", may be NULL
} ConsoleCommand;

// The built-in command typed as `name`, or NULL
const ConsoleCommand* console_command_find(const char* name);

// Every registered command, in id order (index == ConsoleCommandId)
uint32_t console_command_count(void);
const ConsoleCommand* console_command_get(uint32_t index);

// Run a command directly. args is a NULL-terminated list of at most 16
// words, with the same meaning as for the console (for CONSOLE_CMD_EXECUTE,
// args[0] is the program path). Counting stops at the first NULL or empty
// word. Returns the command's result, or CONSOLE_CMD_NOT_FOUND for an
// unknown id.
//
// CONSOLE_CMD_EXECUTE starts the program as a process and blocks until it
// ends (Ctrl+C kills it), so call it from the console thread only.
int console_command_run(ConsoleCommandId id, char** args);

#endif
