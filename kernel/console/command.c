#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/kernel.h>
#include <kernel/knode.h>
#include <kernel/malloc.h>
#include <kernel/fs/fs.h>
#include <kernel/vfs/vfs.h>

#include <kernel/console/command.h>
#include <kernel/console/console.h>
#include <kernel/console/display.h>
#include <kernel/console/print.h>

#include <kernel/util/string.h>

#ifdef KERNEL_PLATFORM_X86
  #include <kernel/arch/x86/heap.h>
#endif

// Command bodies. Each header defines its console_cmd_* function and is
// included only here.
#include <kernel/console/commands/help.h>
#include <kernel/console/commands/list.h>
#include <kernel/console/commands/chdir.h>
#include <kernel/console/commands/clear.h>
#include <kernel/console/commands/execute.h>

#include <kernel/console/commands/mkfile.h>
#include <kernel/console/commands/mkdir.h>
#include <kernel/console/commands/remove.h>
#include <kernel/console/commands/rename.h>
#include <kernel/console/commands/copy.h>
#include <kernel/console/commands/chmod.h>

#include <kernel/console/commands/type.h>
#include <kernel/console/commands/cat.h>
#include <kernel/console/commands/grep.h>
#include <kernel/console/commands/wc.h>
#include <kernel/console/commands/echo.h>

#include <kernel/console/commands/format.h>
#include <kernel/console/commands/fdisk.h>

#include <kernel/console/commands/chkdsk.h>
#include <kernel/console/commands/boot.h>
#include <kernel/console/commands/lsusb.h>
#include <kernel/console/commands/lsblk.h>
#include <kernel/console/commands/ps.h>
#include <kernel/console/commands/kill.h>

// Indexed by id, so console_command_run() needs no search and a missing or
// misplaced row is caught at compile time (array size) or by the NULL check.
static const ConsoleCommand command_table[] = {
    [CONSOLE_CMD_EXECUTE] = { "",       CONSOLE_CMD_EXECUTE, console_cmd_execute_default, 0,                NULL },
    [CONSOLE_CMD_HELP]    = { "help",   CONSOLE_CMD_HELP,    console_cmd_help,    CONSOLE_CMD_FLAG_BUILTIN, "list commands" },
    [CONSOLE_CMD_CLS]     = { "cls",    CONSOLE_CMD_CLS,     console_cmd_clear,   CONSOLE_CMD_FLAG_BUILTIN, "clear the screen" },
    [CONSOLE_CMD_LIST]    = { "ls",     CONSOLE_CMD_LIST,    console_cmd_lsdir,   CONSOLE_CMD_FLAG_BUILTIN, "ls [dir], list a directory" },
    [CONSOLE_CMD_CHDIR]   = { "cd",     CONSOLE_CMD_CHDIR,   console_cmd_chdir,   CONSOLE_CMD_FLAG_BUILTIN, "cd <dir>, cd prints the path" },

    [CONSOLE_CMD_MAKE]    = { "mk",     CONSOLE_CMD_MAKE,    console_cmd_mk,      CONSOLE_CMD_FLAG_BUILTIN, "mk <file> [size]" },
    [CONSOLE_CMD_MAKEDIR] = { "mkdir",  CONSOLE_CMD_MAKEDIR, console_cmd_mkdir,   CONSOLE_CMD_FLAG_BUILTIN, "mkdir <dir>" },
    [CONSOLE_CMD_REMOVE]  = { "rm",     CONSOLE_CMD_REMOVE,  console_cmd_rm,      CONSOLE_CMD_FLAG_BUILTIN, "rm <path>" },
    [CONSOLE_CMD_RENAME]  = { "rn",     CONSOLE_CMD_RENAME,  console_cmd_rename,  CONSOLE_CMD_FLAG_BUILTIN, "rn <path> <new name>" },
    [CONSOLE_CMD_COPY]    = { "cp",     CONSOLE_CMD_COPY,    console_cmd_copy,    CONSOLE_CMD_FLAG_BUILTIN, "cp <path> <path or dir>" },
    [CONSOLE_CMD_CHMOD]   = { "chmod",  CONSOLE_CMD_CHMOD,   console_cmd_chmod,   CONSOLE_CMD_FLAG_BUILTIN, "chmod [+|-|=]<xrw> <path...>" },

    [CONSOLE_CMD_TYPE]    = { "type",   CONSOLE_CMD_TYPE,    console_cmd_type,    CONSOLE_CMD_FLAG_BUILTIN, "type <file>, print it" },
    [CONSOLE_CMD_CAT]     = { "cat",    CONSOLE_CMD_CAT,     console_cmd_cat,     CONSOLE_CMD_FLAG_BUILTIN, "cat <file...> [> or >> <dest>], join files" },
    [CONSOLE_CMD_GREP]    = { "grep",   CONSOLE_CMD_GREP,    console_cmd_grep,    CONSOLE_CMD_FLAG_BUILTIN, "grep [-invc] <text> <file...>, find lines" },
    [CONSOLE_CMD_WC]      = { "wc",     CONSOLE_CMD_WC,      console_cmd_wc,      CONSOLE_CMD_FLAG_BUILTIN, "wc <file...>, count lines, words, bytes" },
    [CONSOLE_CMD_ECHO]    = { "echo",   CONSOLE_CMD_ECHO,    console_cmd_echo,    CONSOLE_CMD_FLAG_BUILTIN, "echo <text> [> or >> <file>]" },

    [CONSOLE_CMD_FORMAT]  = { "format", CONSOLE_CMD_FORMAT,  console_cmd_format,  CONSOLE_CMD_FLAG_BUILTIN, "format <device> [-q] [-t<fs>] [-k<KB>|-M<MB>] [-s<size>]" },
    [CONSOLE_CMD_FDISK]   = { "fdisk",  CONSOLE_CMD_FDISK,   console_cmd_fdisk,   CONSOLE_CMD_FLAG_BUILTIN, "fdisk <disk> [new|del|grow|shrink|active], partition table" },
    [CONSOLE_CMD_CHKDSK]  = { "chkdsk", CONSOLE_CMD_CHKDSK,  console_cmd_chkdsk,  CONSOLE_CMD_FLAG_BUILTIN, "chkdsk <device> [-f], check a volume (-f fixes)" },

    [CONSOLE_CMD_BOOT]    = { "boot",   CONSOLE_CMD_BOOT,    console_cmd_boot,    CONSOLE_CMD_FLAG_BUILTIN, "boot install <disk|partition> | boot repair" },
    [CONSOLE_CMD_LSUSB]   = { "lsusb",  CONSOLE_CMD_LSUSB,   console_cmd_lsusb,   CONSOLE_CMD_FLAG_BUILTIN, "list USB devices" },
    [CONSOLE_CMD_LSBLK]   = { "lsblk",  CONSOLE_CMD_LSBLK,   console_cmd_lsblk,   CONSOLE_CMD_FLAG_BUILTIN, "lsblk [-b] [disk...], list disks and partitions" },
    [CONSOLE_CMD_PS]      = { "ps",     CONSOLE_CMD_PS,      console_cmd_ps,      CONSOLE_CMD_FLAG_BUILTIN, "list running processes" },
    [CONSOLE_CMD_KILL]    = { "kill",   CONSOLE_CMD_KILL,    console_cmd_kill,    CONSOLE_CMD_FLAG_BUILTIN, "kill <pid...>, end processes" }
};

_Static_assert(sizeof(command_table) / sizeof(command_table[0]) == CONSOLE_CMD_COUNT,
               "every ConsoleCommandId needs a row in command_table");

#define CONSOLE_CMD_ARGS_MAX  16

const ConsoleCommand* console_command_find(const char* name) {
    if (name == NULL || name[0] == '\0')
        return NULL;

    for (uint32_t i = 0; i < CONSOLE_CMD_COUNT; i++) {
        const ConsoleCommand* cmd = &command_table[i];
        if (!(cmd->flags & CONSOLE_CMD_FLAG_BUILTIN) || cmd->function == NULL)
            continue;
        if (strcmp(cmd->name, name) == 0)
            return cmd;
    }
    return NULL;
}

uint32_t console_command_count(void) {
    return CONSOLE_CMD_COUNT;
}

const ConsoleCommand* console_command_get(uint32_t index) {
    if (index >= CONSOLE_CMD_COUNT)
        return NULL;
    return &command_table[index];
}

int console_command_run(ConsoleCommandId id, char** args) {
    if ((uint32_t)id >= CONSOLE_CMD_COUNT || command_table[id].function == NULL)
        return CONSOLE_CMD_NOT_FOUND;

    int arg_count = 0;
    if (args != NULL) {
        while (arg_count < CONSOLE_CMD_ARGS_MAX &&
               args[arg_count] != NULL && args[arg_count][0] != '\0')
            arg_count++;
    }

    return command_table[id].function(arg_count, args);
}

void console_command_end(void) {
    // A line printed exactly to the right edge has its wrap pending: this
    // "\n" resolves it as the one line break, as print() promises
    if (display_cursor_get_position() != 0)
        print("\n");
    print("\n");
}




