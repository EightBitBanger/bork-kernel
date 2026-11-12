#ifndef _KERNEL_SYS_FORMAT_H_
#define _KERNEL_SYS_FORMAT_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/syscall/calls/sys_user.h>
#include <kernel/syscall/calls/sys_security.h>
#include <kernel/scheduler/process.h>
#include <kernel/arch/x86/drivers/serial.h>
#include <kernel/fs/format.h>
#include <kernel/util/string.h>
#include <kernel/util/snprint.h>

//
// format(device, SysFormatSpec*) -> 0
//
// ABI in syscall.h ("Formatting"), the work in fs/format.c, shared with the
// kernel console's format command. syscall_dispatch() has already required
// SYS_SEC_FILE_READ; writing needs SYS_SEC_ADMIN and SYS_SEC_FILE_WRITE on
// top, checked here before anything else is looked at.
//
// Everything from the caller is copied into kernel memory once, up front,
// and checked there: the device name, the whole spec and the filesystem
// name in it. Nothing is read from user memory again while the format
// runs, and the result is written back only after it is over (with no lock
// held), so a bad or changing pointer can only cost the caller its answer.
//

_Static_assert(sizeof(SysFormatSpec) == 64, "SysFormatSpec is part of the user ABI");
_Static_assert(SYS_FORMAT_FS_MAX == FORMAT_FS_NAME_MAX, "SYS_FORMAT_FS_MAX must equal FORMAT_FS_NAME_MAX");
_Static_assert(BLOCK_NAME_MAX == VFS_DEVICE_NAME_MAX, "device names are BLOCK_NAME_MAX long");

// A device name as block devices are named: 1..BLOCK_NAME_MAX-1 lowercase
// letters and digits. Rules out paths, "..", spaces and control characters.
static bool sys_format_name_valid(const char* name) {
    if (name[0] == '\0')
        return false;
    for (const char* p = name; *p != '\0'; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')))
            return false;
    }
    return true;
}

static bool sys_format_block_valid(uint32_t block_size) {
    if (block_size == 0)
        return true;        // Default
    return block_size >= SYS_FORMAT_BLOCK_MIN && block_size <= SYS_FORMAT_BLOCK_MAX &&
           (block_size & (block_size - 1)) == 0;
}

static int32_t sys_format_errno(FormatResult result) {
    switch (result) {
        case FORMAT_OK:             return SYS_OK;
        case FORMAT_NO_DEVICE:
        case FORMAT_NOT_MOUNTED:    return SYS_ENOENT;
        case FORMAT_BAD_FILESYSTEM:
        case FORMAT_BAD_BLOCK_SIZE:
        case FORMAT_TOO_SMALL:      return SYS_EINVAL;
        case FORMAT_TOO_LARGE:      return SYS_ENOSPC;
        case FORMAT_SYSTEM_DEVICE:  return SYS_EACCES;
        case FORMAT_BUSY:
        case FORMAT_IN_PROGRESS:    return SYS_EBUSY;
        case FORMAT_WIPE_FAILED:
        case FORMAT_IO_ERROR:       return SYS_EIO;
    }
    return SYS_EIO;
}

// One COM1 line, written in one go so other threads' lines don't split it:
// "format: pid 7 (format) ata1 whole disk, 134217728 bytes, 512-byte blocks, quick: <what>"
static void sys_format_log(const char* device, const FormatPlan* plan, bool quick, const char* what) {
    ProcessId pid = process_current_id();
    ProcessInfo info;
    bool known = ProcessGetInfo(pid, &info);

    char line[192];
    int length = snprintf(line, sizeof(line), "format: pid %d (%s) %s", (int)pid, known ? info.name : "?", device);
    if (plan->device[0] != '\0' && length > 0 && (size_t)length < sizeof(line))
        length += snprintf(line + length, sizeof(line) - (size_t)length, "%s%llu bytes, %u-byte blocks",
                           plan->whole_disk ? " whole disk, " : ", ",
                           (unsigned long long)plan->capacity, (unsigned)plan->block_size);
    if (length > 0 && (size_t)length < sizeof(line))
        length += snprintf(line + length, sizeof(line) - (size_t)length, "%s%s\n", quick ? ", quick: " : ", full: ", what);
    if (length < 0)
        return;
    if ((size_t)length >= sizeof(line)) {
        length = (int)sizeof(line) - 1;
        line[length - 1] = '\n';
    }
    serial_write(line, (uint32_t)length);
}

// Logged once the format lock is held and the plan checked again, right
// before the first write: a line that says "started" with no result after
// it means the machine went down part way
static void sys_format_started(const FormatPlan* plan) {
    sys_format_log(plan->device, plan, plan->quick, "started");
}

static int32_t sys_format(const SyscallArgs* args) {
    AddressSpace* space = process_current_space();
    if (space == NULL)
        return SYS_EFAULT;

    // The spec, whole, into kernel memory
    SysFormatSpec spec;
    if (!vmm_user_check(space, args->arg2, sizeof(spec), false) ||
        !vmm_user_copy_from(space, &spec, args->arg2, sizeof(spec)))
        return SYS_EFAULT;
    if (spec.size != sizeof(SysFormatSpec) || spec.reserved != 0 ||
        (spec.flags & ~(uint32_t)SYS_FORMAT_FLAGS) != 0)
        return SYS_EINVAL;

    bool query = (spec.flags & SYS_FORMAT_QUERY) != 0;
    bool quick = (spec.flags & SYS_FORMAT_QUICK) != 0;

    // Writing: admin and file write, before anything else is looked at
    if (!query && !sys_security_allows(SYS_SEC_ADMIN | SYS_SEC_FILE_WRITE))
        return SYS_EPERM;

    char device[BLOCK_NAME_MAX];
    int32_t length = sys_user_string(space, args->arg1, device, sizeof(device));
    if (length == SYS_ENAMETOOLONG)
        return SYS_EINVAL;
    if (length < 0)
        return length;
    if (!sys_format_name_valid(device))
        return SYS_EINVAL;

    // The filesystem name must end inside its field
    if (strnlen(spec.filesystem, sizeof(spec.filesystem)) == sizeof(spec.filesystem))
        return SYS_EINVAL;
    if (!sys_format_block_valid(spec.block_size))
        return SYS_EINVAL;

    FormatRequest request = {
        .filesystem   = spec.filesystem,
        .capacity     = spec.capacity,
        .block_size   = spec.block_size,
        .quick        = quick,
        .quiet        = true,       // The desktop owns the screen
        .allow_system = false,      // Never from user space
    };

    FormatPlan plan;
    FormatResult result;
    if (query) {
        result = format_plan(device, &request, &plan);
    } else {
        // Every attempt by an admin is logged with its outcome, except
        // "another format is running", which a retry loop would repeat
        // as fast as it can
        request.started = sys_format_started;
        result = format_run(device, &request, &plan, NULL);
        if (result != FORMAT_IN_PROGRESS)
            sys_format_log(device, &plan, quick, format_result_text(result));
    }

    // Said how it went: what it would do, or did. Also for a size that
    // does not fit or the system device, so the caller can tell the user.
    if (result == FORMAT_OK || result == FORMAT_TOO_LARGE || result == FORMAT_SYSTEM_DEVICE) {
        SysFormatSpec out;
        memset(&out, 0, sizeof(out));
        out.size               = sizeof(out);
        out.flags              = spec.flags;
        out.block_size         = plan.block_size;
        out.capacity           = plan.capacity;
        out.max_capacity       = plan.max_capacity;
        out.device_bytes       = plan.device_bytes;
        out.device_sector_size = plan.device_sector_size;
        out.info               = (plan.whole_disk ? SYS_FORMAT_INFO_WHOLE_DISK : 0) |
                                 (plan.formatted  ? SYS_FORMAT_INFO_FORMATTED  : 0) |
                                 (plan.system     ? SYS_FORMAT_INFO_SYSTEM     : 0);
        memcpy(out.filesystem, plan.filesystem, sizeof(out.filesystem));
        out.filesystem[sizeof(out.filesystem) - 1] = '\0';
        if (!sys_user_put(space, args->arg2, &out, sizeof(out)))
            return (result == FORMAT_OK && query) ? SYS_EFAULT : sys_format_errno(result);
    }
    return sys_format_errno(result);
}

#endif
