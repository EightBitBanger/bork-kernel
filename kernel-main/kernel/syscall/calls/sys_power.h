#ifndef _KERNEL_SYS_POWER_H_
#define _KERNEL_SYS_POWER_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/console/print.h>
#include <kernel/vfs/vfs.h>
#include <kernel/util/system.h>

//
// power(op) -> does not return (SYS_EINVAL for an unknown op)
//
//   SYS_POWER_OFF       write every file system out, then ACPI S5
//   SYS_POWER_RESTART   write every file system out, then reset
//
// Needs SYS_SEC_ADMIN (syscall_dispatch checks). The registry is saved on
// every change already, so the file systems are all there is to write.
// Running programs are not told: the desktop closes its windows first.
// If the machine stays on after S5 (no ACPI tables, or the firmware
// ignored it), the console says it is safe to switch off and the CPU halts.
//

static int32_t sys_power(const SyscallArgs* args) {
    uint32_t op = args->arg1;
    if (op != SYS_POWER_OFF && op != SYS_POWER_RESTART)
        return SYS_EINVAL;

    vfs_lock();
    (void)vfs_sync_all();           // Nothing to do about a failure now
    vfs_unlock();

    if (op == SYS_POWER_RESTART)
        system_restart();

    system_poweroff();              // Back only if the power is still on
    print("\nIt is now safe to turn off your computer.\n");
    for (;;)
        __asm__ volatile ("cli; hlt");
    return SYS_OK;
}

#endif
