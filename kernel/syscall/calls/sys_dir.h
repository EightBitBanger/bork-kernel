#ifndef _KERNEL_SYS_DIR_H_
#define _KERNEL_SYS_DIR_H_

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/syscall/calls/sys_files.h>
#include <kernel/syscall/calls/sys_user.h>
#include <kernel/vfs/vfs.h>
#include <kernel/scheduler/process.h>
#include <kernel/util/string.h>

//
// Directory calls: readdir, chdir, getcwd
//

//
// readdir(path, index, buf, size) -> name length, 0 past the last entry
//
// Entries are numbered from 0. Each call takes the VFS lock once, so a
// directory that changes between calls can skip or repeat an entry.
// SYS_EIO means that one entry could not be read: carry on at index + 1.
//

static int32_t sys_readdir_locked(SysProcessFiles* t, const SyscallArgs* args) {
    AddressSpace* space = process_current_space();
    uint32_t index       = args->arg2;
    uint32_t user_buffer = args->arg3;
    uint32_t size        = args->arg4;

    char path[SYS_PATH_MAX];
    int32_t result = sys_user_path(space, t->cwd, args->arg1, path);
    if (result < 0)
        return result;

    char name[SYS_NAME_MAX + 1];
    bool have_name = false;

    vfs_lock();
    if (!vfs_exists(path))
        result = SYS_ENOENT;
    else if (!vfs_directory_check(path))
        result = SYS_ENOTDIR;
    else if (index >= vfs_directory_get_item_count(path))
        result = 0;
    else if (!vfs_directory_get_item(path, index, name, sizeof(name)) || name[0] == '\0')
        result = SYS_EIO;
    else
        have_name = true;
    vfs_unlock();

    if (!have_name)
        return result;

    uint32_t length = (uint32_t)strlen(name);
    if (size < length + 1)
        return SYS_ERANGE;
    if (!sys_user_put(space, user_buffer, name, length + 1))
        return SYS_EFAULT;
    return (int32_t)length;
}

static int32_t sys_readdir(const SyscallArgs* args) {
    return sys_files_call(sys_readdir_locked, args);
}

//
// chdir(path)
//

static int32_t sys_chdir_locked(SysProcessFiles* t, const SyscallArgs* args) {
    char path[SYS_PATH_MAX];
    int32_t result = sys_user_path(process_current_space(), t->cwd, args->arg1, path);
    if (result < 0)
        return result;

    vfs_lock();
    if (!vfs_exists(path))
        result = SYS_ENOENT;
    else if (!vfs_directory_check(path))
        result = SYS_ENOTDIR;
    else
        result = SYS_OK;
    vfs_unlock();

    if (result == SYS_OK)
        memcpy(t->cwd, path, strlen(path) + 1);     // Normalized: fits
    return result;
}

static int32_t sys_chdir(const SyscallArgs* args) {
    return sys_files_call(sys_chdir_locked, args);
}

//
// getcwd(buf, size) -> length (the terminator is written too)
//

static int32_t sys_getcwd_locked(SysProcessFiles* t, const SyscallArgs* args) {
    uint32_t length = (uint32_t)strlen(t->cwd);
    if (args->arg2 < length + 1)
        return SYS_ERANGE;
    if (!sys_user_put(process_current_space(), args->arg1, t->cwd, length + 1))
        return SYS_EFAULT;
    return (int32_t)length;
}

static int32_t sys_getcwd(const SyscallArgs* args) {
    return sys_files_call(sys_getcwd_locked, args);
}

#endif
