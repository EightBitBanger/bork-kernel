#ifndef CONSOLE_CMD_POWER_H
#define CONSOLE_CMD_POWER_H

#include <stdint.h>

#include <kernel/kernel.h>
#include <kernel/console/print.h>
#include <kernel/vfs/vfs.h>
#include <kernel/util/system.h>

// shutdown   write every file system out, then turn the machine off
// restart    write every file system out, then reset it
//
// The same as the desktop's buttons (SYS_POWER), from the console.

static void console_power_sync(void) {
    print("Writing file systems out...\n");
    vfs_lock();
    bool ok = vfs_sync_all();
    vfs_unlock();
    if (!ok)
        print("A device refused a write; going on anyway\n");
}

int console_cmd_shutdown(int arg_count, char** args) {
    (void)arg_count;
    (void)args;
    console_power_sync();
    system_poweroff();          // Back only if the power is still on
    print("It is now safe to turn off your computer.\n");
    for (;;)
        __asm__ volatile ("cli; hlt");
    return 0;
}

int console_cmd_restart(int arg_count, char** args) {
    (void)arg_count;
    (void)args;
    console_power_sync();
    system_restart();
    return 0;
}

#endif
