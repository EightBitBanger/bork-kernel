#ifndef _KERNEL_SYS_UNMOUNT_H_
#define _KERNEL_SYS_UNMOUNT_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/syscall/calls/sys_user.h>
#include <kernel/scheduler/process.h>
#include <kernel/arch/x86/drivers/serial.h>
#include <kernel/block/block.h>
#include <kernel/block/partition.h>
#include <kernel/fs/format.h>
#include <kernel/vfs/vfs.h>
#include <kernel/util/string.h>
#include <kernel/util/snprint.h>

//
// unmount(device) -> 0
//
// ABI in syscall.h ("Unmounting"). The work is vfs_device_unmount_disk:
// sync, flush the drive, detach, drop the /mnt entries. A partition's name
// unmounts the whole disk it is on, every partition included: the point is
// to pull the stick out. syscall_dispatch() has already required
// SYS_SEC_FILE_WRITE.
//
// The device holding the system folders, and the disk they are on, are
// never unmounted (SYS_EACCES). It runs under the format lock, so never in
// the middle of a format, an fdisk or a boot install.
//

// A device name as block devices are named: lowercase letters and digits
static bool sys_unmount_name_valid(const char* name) {
    if (name[0] == '\0')
        return false;
    for (const char* p = name; *p != '\0'; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')))
            return false;
    }
    return true;
}

// "unmount: pid 7 (dwm) usb0: ok", one COM1 line in one write
static void sys_unmount_log(const char* device, const char* what) {
    ProcessId pid = process_current_id();
    ProcessInfo info;
    bool known = ProcessGetInfo(pid, &info);
    char line[128];
    int length = snprintf(line, sizeof(line), "unmount: pid %d (%s) %s: %s\n",
                          (int)pid, known ? info.name : "?", device, what);
    if (length <= 0)
        return;
    if ((size_t)length >= sizeof(line)) {
        length = (int)sizeof(line) - 1;
        line[length - 1] = '\n';
    }
    serial_write(line, (uint32_t)length);
}

static int32_t sys_unmount(const SyscallArgs* args) {
    AddressSpace* space = process_current_space();
    if (space == NULL)
        return SYS_EFAULT;

    char device[BLOCK_NAME_MAX];
    int32_t length = sys_user_string(space, args->arg1, device, sizeof(device));
    if (length == SYS_ENAMETOOLONG)
        return SYS_EINVAL;
    if (length < 0)
        return length;
    if (!sys_unmount_name_valid(device))
        return SYS_EINVAL;

    struct BlockDevice* named = block_find(device);
    if (named == NULL)
        return SYS_ENOENT;
    struct BlockDevice* disk = block_partition_parent(named);
    if (disk == NULL)
        disk = named;           // Already the whole disk

    // The whole disk counts: a partition next to home is still on the
    // system's disk
    if (format_device_is_system(disk)) {
        sys_unmount_log(device, "refused, the system disk");
        return SYS_EACCES;
    }

    if (!format_lock_try())
        return SYS_EBUSY;       // A format, fdisk or boot install is running
    VfsFormatResult result = vfs_device_unmount_disk(disk);
    format_lock_release();

    switch (result) {
        case VFS_FORMAT_OK:
            sys_unmount_log(device, "unmounted, safe to remove");
            return SYS_OK;
        case VFS_FORMAT_NOT_MOUNTED:
            return SYS_ENOENT;
        case VFS_FORMAT_BUSY:
            sys_unmount_log(device, "busy, files are open");
            return SYS_EBUSY;
        default:
            sys_unmount_log(device, "sync failed, left mounted");
            return SYS_EIO;
    }
}

#endif
