#ifndef CONSOLE_CMD_EXECUTE_H
#define CONSOLE_CMD_EXECUTE_H

#include <stdint.h>
#include <stdbool.h>

#include <kernel/config.h>
#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/scheduler/process.h>
#include <kernel/boot/x86/interrupt.h>      // input_key_pop
#include <kernel/console/keyboard.h>        // kb_ctrl_down
#include <kernel/console/virtual_key.h>     // kb_vkey_check, VK_C
#include <kernel/console/command.h>         // CONSOLE_CMD_NOT_FOUND
#include <kernel/console/commands/path_util.h>

// How often the console checks whether the program is still running (and
// whether Ctrl+C is held) while it waits
#define CONSOLE_EXEC_POLL_MS     20
#define CONSOLE_EXEC_PRIORITY    PRIORITY_NORMAL

// Wait for a foreground program to end. Ctrl+C kills it, unless
// `breakable` is false (CONSOLE_THREAD_NO_BREAK).
//
// Ctrl+C is read from the live key state (the virtual key map), not the key
// queue, so it works even while the program holds the input grab and the
// keys go to it instead of the console.
static void console_exec_wait(ProcessId pid, bool breakable) {
    bool killed = false;
    ProcessInfo info;

    // ProcessGetInfo fails once the slot is freed: every way a process ends
    // (exit, fault, DestroyProcess) goes through DestroyProcess, which frees
    // it, and the generation in the pid keeps a reused slot from matching
    while (ProcessGetInfo(pid, &info)) {
        if (breakable && !killed && kb_ctrl_down() && kb_vkey_check(VK_C)) {
            killed = true;
            DestroyProcess(pid);
            print("^C\n");
            continue;
        }
        thread_sleep(CONSOLE_EXEC_POLL_MS);
    }

    // The 'c' of Ctrl+C (and anything typed with it) is queued for the
    // console; don't let it land on the next prompt
    if (killed) {
        uint16_t key;
        while (input_key_pop(&key)) { }
    }
}

int console_cmd_execute(int arg_count, char** args, uint32_t security, uint8_t flags) {
    if (arg_count < 1 || args == NULL || args[0] == NULL)
        return CONSOLE_CMD_NOT_FOUND;

    char path[VFS_PATH_MAX];
    if (!command_resolve_path(args[0], path, sizeof(path)))
        return CONSOLE_CMD_NOT_FOUND;

    // Directories and mounts are not programs
    FSFileStats stats;
    if (!vfs_stat(path, &stats) || stats.type != VFS_TYPE_FILE)
        return CONSOLE_CMD_NOT_FOUND;

    // A file by that name exists: say why it can't run instead of
    if (!(stats.permissions & VFS_PERMISSION_EXECUTE)) {
        print(path);
        print(": not marked executable\n");
        return 2;
    }

    // Bits the kernel does not define would be refused by the loader
    // anyway
    if ((security & ~SYS_SEC_VALID) != 0) {
        print(path);
        print(": invalid security mask\n");
        return 4;
    }

    char cwd[VFS_PATH_MAX];
    kernel_get_working_directory(cwd, sizeof(cwd));

    // argv[0] is the resolved path (what every kernel launch passes), the
    // rest are the words typed after the command, as they are. The loader
    // copies all of them onto the new process's stack before it returns,
    // so pointing into the caller's buffers is fine.
    if (arg_count > PROCESS_ARG_MAX) {
        print(path);
        print(": ");
        print(process_error_string(PROCESS_ERR_TOO_BIG));
        print("\n");
        return 5;
    }

    const char* argv[PROCESS_ARG_MAX];
    argv[0] = path;
    for (int i = 1; i < arg_count; i++)
        argv[i] = (args[i] != NULL) ? args[i] : "";

    ProcessLaunch launch = {
        .image_path = path,
        .cwd        = cwd,
        .priority   = CONSOLE_EXEC_PRIORITY,
        .security   = (ProcessSecurity)security,
        .argc       = (uint32_t)arg_count,
        .argv       = argv,
    };
    ProcessId pid = CreateProcessLaunch(&launch);

    /*
    // No process error messages yet, maybe later on
    if (pid <= 0) {
        print(path);
        print(": ");
        print(process_error_string(pid));
        print("\n");
        return 3;
    }
    */

    if (flags & CONSOLE_THREAD_BLOCK)
        console_exec_wait(pid, !(flags & CONSOLE_THREAD_NO_BREAK));

    return 0;
}

// What the console uses for a program typed at the prompt (command table entry).
// CONFIG_CONSOLE_FULL_CLEARANCE (kernel/config.h) chooses between every bit
// and the user default.
int console_cmd_execute_default(int arg_count, char** args) {
#if CONFIG_CONSOLE_FULL_CLEARANCE
    uint32_t security = SYS_SEC_VALID;
#else
    uint32_t security = PROCESS_SECURITY_DEFAULT;
#endif
    int result = console_cmd_execute(arg_count, args, security, CONSOLE_THREAD_BLOCK);

    // Not found: the console tries the next place, nothing was printed
    if (result != CONSOLE_CMD_NOT_FOUND)
        console_command_end();
    return result;
}

#endif
