
#ifndef _KERNEL_SYS_PATH_H_
#define _KERNEL_SYS_PATH_H_

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/syscall/calls/sys_files.h>
#include <kernel/syscall/calls/sys_user.h>
#include <kernel/vfs/vfs.h>
#include <kernel/scheduler/process.h>
#include <kernel/util/string.h>

//
// Path calls: stat, mkdir, remove, rename, truncate, copy
//
// Each check-then-act sequence runs under vfs_lock() (recursive), so the
// error a call reports matches the state it acted on. copy is the exception:
// vfs_copy deliberately does not hold the lock for a whole tree.
//

//
// stat(path, SysStat*)
//

static int32_t sys_stat_locked(SysProcessFiles* t, const SyscallArgs* args) {
    AddressSpace* space = process_current_space();
    char path[SYS_PATH_MAX];
    int32_t result = sys_user_path(space, t->cwd, args->arg1, path);
    if (result < 0)
        return result;

    FSFileStats st;
    if (!vfs_stat(path, &st))
        return SYS_ENOENT;
    return sys_stat_to_user(space, args->arg2, &st);
}

static int32_t sys_stat(const SyscallArgs* args) {
    return sys_files_call(sys_stat_locked, args);
}

//
// mkdir(path)
//

static int32_t sys_mkdir_locked(SysProcessFiles* t, const SyscallArgs* args) {
    char path[SYS_PATH_MAX];
    int32_t result = sys_user_path(process_current_space(), t->cwd, args->arg1, path);
    if (result < 0)
        return result;

    vfs_lock();
    if (vfs_exists(path))
        result = SYS_EEXIST;
    else if (!sys_parent_is_directory(path))
        result = SYS_ENOENT;
    else if (!vfs_mkdir(path))
        result = SYS_EACCES;    // Permissions, bad name, or the device is full
    else
        result = SYS_OK;
    vfs_unlock();
    return result;
}

static int32_t sys_mkdir(const SyscallArgs* args) {
    return sys_files_call(sys_mkdir_locked, args);
}

//
// remove(path): a file or an EMPTY directory
//
// vfs_remove would take a whole tree with it. One call from user space
// should not be able to do that, so a directory must be emptied first.
//

static int32_t sys_remove_locked(SysProcessFiles* t, const SyscallArgs* args) {
    char path[SYS_PATH_MAX];
    int32_t result = sys_user_path(process_current_space(), t->cwd, args->arg1, path);
    if (result < 0)
        return result;

    vfs_lock();
    if (!vfs_exists(path))
        result = SYS_ENOENT;
    else if (vfs_directory_check_mounted(path))
        result = SYS_EBUSY;
    else if (sys_files_path_open(t, path))
        result = SYS_EBUSY;
    else if (vfs_directory_check(path) && vfs_directory_get_item_count(path) != 0)
        result = SYS_ENOTEMPTY;
    else if (!vfs_remove(path))
        result = SYS_EACCES;    // Permissions, or another process has it open
    else
        result = SYS_OK;
    vfs_unlock();
    return result;
}

static int32_t sys_remove(const SyscallArgs* args) {
    return sys_files_call(sys_remove_locked, args);
}

//
// rename(path, new_name): new name in the same directory
//

static int32_t sys_rename_locked(SysProcessFiles* t, const SyscallArgs* args) {
    AddressSpace* space = process_current_space();
    char path[SYS_PATH_MAX];
    char name[SYS_NAME_MAX + 1];

    int32_t result = sys_user_path(space, t->cwd, args->arg1, path);
    if (result < 0)
        return result;
    result = sys_user_name(space, args->arg2, name);
    if (result < 0)
        return result;
    if (strcmp(path, "/") == 0)
        return SYS_EBUSY;

    char renamed[SYS_PATH_MAX];
    if (!sys_sibling_path(path, name, renamed, sizeof(renamed)))
        return SYS_ENAMETOOLONG;

    vfs_lock();
    if (!vfs_exists(path))
        result = SYS_ENOENT;
    else if (vfs_directory_check_mounted(path))
        result = SYS_EBUSY;     // Mounts are named after their device
    else if (strcmp(renamed, path) == 0)
        result = SYS_OK;
    else if (vfs_exists(renamed))
        result = SYS_EEXIST;
    else if (!vfs_rename(path, name))
        result = SYS_EACCES;    // Permissions, or a name too long for a kernel node
    else {
        sys_files_rename_paths(t, path, renamed);
        result = SYS_OK;
    }
    vfs_unlock();
    return result;
}

static int32_t sys_rename(const SyscallArgs* args) {
    return sys_files_call(sys_rename_locked, args);
}

//
// truncate(path, size): shrink or grow a file
//

static int32_t sys_truncate_locked(SysProcessFiles* t, const SyscallArgs* args) {
    char path[SYS_PATH_MAX];
    int32_t result = sys_user_path(process_current_space(), t->cwd, args->arg1, path);
    if (result < 0)
        return result;

    FSFileStats st;
    vfs_lock();
    if (!vfs_stat(path, &st))
        result = SYS_ENOENT;
    else if (st.type != VFS_TYPE_FILE)
        result = SYS_EISDIR;
    else if (!(st.permissions & VFS_PERMISSION_WRITE))
        result = SYS_EACCES;
    else if (!vfs_truncate(path, args->arg2))
        result = SYS_ENOSPC;    // No space, or not a file on a storage device
    else
        result = SYS_OK;
    vfs_unlock();
    return result;
}

static int32_t sys_truncate(const SyscallArgs* args) {
    return sys_files_call(sys_truncate_locked, args);
}

//
// copy(src, dest): a file or a directory tree, to a new path
//

static int32_t sys_copy_locked(SysProcessFiles* t, const SyscallArgs* args) {
    AddressSpace* space = process_current_space();
    char src[SYS_PATH_MAX];
    char dest[SYS_PATH_MAX];

    int32_t result = sys_user_path(space, t->cwd, args->arg1, src);
    if (result < 0)
        return result;
    result = sys_user_path(space, t->cwd, args->arg2, dest);
    if (result < 0)
        return result;

    if (!vfs_exists(src))
        return SYS_ENOENT;
    if (vfs_directory_check_mounted(src))
        return SYS_EBUSY;
    if (sys_path_within(dest, src))
        return SYS_EINVAL;      // Into itself
    if (vfs_exists(dest))
        return SYS_EEXIST;
    if (!sys_parent_is_directory(dest))
        return SYS_ENOENT;

    // On failure vfs_copy removes whatever it made at dest
    return vfs_copy(src, dest) ? SYS_OK : SYS_EIO;
}

static int32_t sys_copy(const SyscallArgs* args) {
    return sys_files_call(sys_copy_locked, args);
}

#endif
