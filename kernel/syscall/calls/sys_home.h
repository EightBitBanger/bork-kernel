#ifndef _KERNEL_SYS_HOME_H_
#define _KERNEL_SYS_HOME_H_

#include <kernel/kernel.h>
#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/syscall/calls/sys_user.h>
#include <kernel/vfs/vfs.h>
#include <kernel/scheduler/process.h>
#include <kernel/util/string.h>

//
// gethome(buf, size) -> path length (terminator not counted)
//
// The mount root of the device holding the system folders ("/mnt/ata0p1"),
// the path the kernel itself builds "<home>/sys" from (boot.c, boot repair).
// Set by the PCI scan when a device with /sys is mounted. Always allowed,
// like getcwd.
//
//   SYS_ENOENT   no device has claimed home, or it is gone (detached, or
//                reformatted so the mount root no longer reads back)
//   SYS_ERANGE   size is smaller than the path plus its terminator
//   SYS_EFAULT   buf is not writable user memory
//
// kernel_get_local_paths() takes the VFS lock; the syscall gate is a trap
// gate, so interrupts are on here and that is safe.
//

static int32_t sys_gethome(const SyscallArgs* args) {
    struct LocalPaths paths;
    kernel_get_local_paths(&paths);

    uint32_t length = (uint32_t)strnlen(paths.home, sizeof(paths.home));
    if (length == 0 || !vfs_directory_check(paths.home))
        return SYS_ENOENT;
    if (args->arg2 < length + 1)
        return SYS_ERANGE;

    AddressSpace* space = process_current_space();
    if (space == NULL || !sys_user_put(space, args->arg1, paths.home, length + 1))
        return SYS_EFAULT;
    return (int32_t)length;
}

#endif
