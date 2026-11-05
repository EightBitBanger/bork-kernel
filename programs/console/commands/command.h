#ifndef CONSOLE_COMMAND_H
#define CONSOLE_COMMAND_H

#include <stdint.h>

// Console command registry (user space copy)
//
// The built-in commands of the kernel console (console/command.c in the
// kernel), copied into the console program so the window console handles
// the same lines. The kernel console is unchanged; this copy is where the
// commands move to as the console becomes a user program.
//
// Each command is a console_cmd_* function in commands/<name>.h, called
// with the words typed after the command name:
//
//   "cp a.txt /mnt/ata0"  ->  console_cmd_copy(2, { "a.txt", "/mnt/ata0" })
//
// Return 0 on success, anything else on failure.
//
// Output goes through the host (console_host.h): print() adds text to the
// window's log. A command ends its output with one blank line by calling
// console_command_end() on every way out (cls excepted: it leaves a clear
// log), as in the kernel.
//
// Commands that need kernel internals with no system call yet (fdisk,
// chkdsk, boot, lsusb, lsblk, kill) are listed with a body that says so;
// they still work in the kernel console.
//
// Adding a command: write commands/<name>.h, include it in
// commands/command.c, add an id below (before CONSOLE_CMD_COUNT) and a row
// to the table there.

typedef int (*ConsoleCommandFn)(int arg_count, char** args);

typedef enum {
    CONSOLE_CMD_HELP,
    CONSOLE_CMD_CLS,
    CONSOLE_CMD_LIST,
    CONSOLE_CMD_CHDIR,
    CONSOLE_CMD_MAKE,
    CONSOLE_CMD_MAKEDIR,
    CONSOLE_CMD_REMOVE,
    CONSOLE_CMD_RENAME,
    CONSOLE_CMD_COPY,
    CONSOLE_CMD_MOVE,
    CONSOLE_CMD_CHMOD,
    CONSOLE_CMD_TYPE,
    CONSOLE_CMD_CAT,
    CONSOLE_CMD_GREP,
    CONSOLE_CMD_WC,
    CONSOLE_CMD_ECHO,
    CONSOLE_CMD_FORMAT,
    CONSOLE_CMD_FDISK,
    CONSOLE_CMD_CHKDSK,
    CONSOLE_CMD_BOOT,
    CONSOLE_CMD_LSUSB,
    CONSOLE_CMD_LSBLK,
    CONSOLE_CMD_PS,
    CONSOLE_CMD_KILL,
    CONSOLE_CMD_NET,
    CONSOLE_CMD_EXIT,
    CONSOLE_CMD_GOTO,

    CONSOLE_CMD_COUNT
} ConsoleCommandId;

// Typed at the prompt by name (and listed by "help")
#define CONSOLE_CMD_FLAG_BUILTIN   0x01

typedef struct {
    const char*      name;      // What the user types
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

#endif
