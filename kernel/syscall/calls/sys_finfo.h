#ifndef _KERNEL_SYS_FINFO_H_
#define _KERNEL_SYS_FINFO_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/syscall/calls/sys_procfile.h>
#include <kernel/syscall/calls/sys_user.h>
#include <kernel/syscall/calls/sys_security.h>
#include <kernel/vfs/vfs.h>
#include <kernel/scheduler/process.h>
#include <kernel/util/string.h>

//
// File details: finfo(path, SysFileInfo*), chmod(path, permissions)
// (ABI in syscall.h, "File details")
//

_Static_assert(sizeof(SysFileInfo) == 32, "SysFileInfo is part of the user ABI");

// ==========================================================================
// VFS adapter. The three calls below are the only VFS functions this file
// uses that the rest of syscall/ doesn't: if your vfs.h names them
// differently, change them here and nothing else.
//
//   vfs_set_permissions(path, uint8_t)       VFS_PERMISSION_* bits (the
//                                            kernel properties window used it)
//   vfs_device_get_used / _capacity(path)    a device root's usage (same)
//   vfs_get_security(path, uint32_t*)        the file's basefs `security`
//                                            mask, the bits vfs_check_access
//                                            compares against the clearance
// ==========================================================================

static inline bool sys_finfo_get_security(const char* path, uint32_t* mask) {
    return vfs_get_security(path, mask);
}

static inline bool sys_finfo_set_permissions(const char* path, uint8_t permissions) {
    return vfs_set_permissions(path, permissions);
}

static inline void sys_finfo_device_usage(const char* path, uint32_t* used, uint32_t* capacity) {
    *used     = vfs_device_get_used(path);
    *capacity = vfs_device_get_capacity(path);
}

// ==========================================================================

#define SYS_PERM_ALL    (SYS_PERM_READ | SYS_PERM_WRITE | SYS_PERM_EXECUTE)

// The folder holding `path` (absolute, normalized) into out
static void sys_finfo_parent(const char* path, char out[SYS_PATH_MAX]) {
    memcpy(out, path, strlen(path) + 1);
    char* slash = strrchr(out, '/');
    if (slash == NULL || slash == out)
        strcpy(out, "/");
    else
        *slash = '\0';
}

// Why the caller may not chmod `path`, or SYS_OK. Caller holds the VFS lock.
static int32_t sys_chmod_refusal(const char* path) {
    if (!vfs_exists(path))
        return SYS_ENOENT;
    if (strcmp(path, "/") == 0 || vfs_directory_check_mounted(path))
        return SYS_EBUSY;
    if (!sys_security_allows(SYS_SEC_FILE_WRITE))
        return SYS_EPERM;
    if (!vfs_check_access(path) || !vfs_check_parent_access(path))
        return SYS_EACCES;

    // As the kernel properties window had it: only in a writable folder
    char parent[SYS_PATH_MAX];
    sys_finfo_parent(path, parent);
    uint8_t parent_perm = 0;
    if (!vfs_get_permissions(parent, &parent_perm) || !(parent_perm & VFS_PERMISSION_WRITE))
        return SYS_EACCES;
    return SYS_OK;
}

//
// finfo(path, SysFileInfo*)
//

static int32_t sys_finfo_locked(SysProcessFiles* t, const SyscallArgs* args) {
    AddressSpace* space = process_current_space();
    char path[SYS_PATH_MAX];
    int32_t result = sys_user_path(space, t->cwd, args->arg1, path);
    if (result < 0)
        return result;

    SysFileInfo info;
    memset(&info, 0, sizeof(info));

    vfs_lock();
    if (!vfs_exists(path)) {
        vfs_unlock();
        return SYS_ENOENT;
    }
    uint32_t mask = 0;
    if (sys_finfo_get_security(path, &mask))
        info.security = mask & VFS_CLEARANCE_ALL;
    if (vfs_check_access(path))
        info.flags |= SYS_FINFO_ACCESS;
    if (sys_chmod_refusal(path) == SYS_OK)
        info.flags |= SYS_FINFO_CHMOD;
    if (vfs_directory_check_mounted(path)) {
        info.flags |= SYS_FINFO_DEVICE;
        sys_finfo_device_usage(path, &info.device_used, &info.device_capacity);
    }
    vfs_unlock();

    return sys_user_put(space, args->arg2, &info, sizeof(info)) ? SYS_OK : SYS_EFAULT;
}

static int32_t sys_finfo(const SyscallArgs* args) {
    return sys_files_call(sys_finfo_locked, args);
}

//
// chmod(path, permissions)
//

static int32_t sys_chmod_locked(SysProcessFiles* t, const SyscallArgs* args) {
    uint32_t permissions = args->arg2;
    if ((permissions & ~SYS_PERM_ALL) != 0)
        return SYS_EINVAL;

    char path[SYS_PATH_MAX];
    int32_t result = sys_user_path(process_current_space(), t->cwd, args->arg1, path);
    if (result < 0)
        return result;

    vfs_lock();
    result = sys_chmod_refusal(path);
    if (result == SYS_OK && !sys_finfo_set_permissions(path, (uint8_t)permissions))
        result = SYS_EIO;
    vfs_unlock();
    return result;
}

static int32_t sys_chmod(const SyscallArgs* args) {
    return sys_files_call(sys_chmod_locked, args);
}

#endif
