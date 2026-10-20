#ifndef _KERNEL_SYS_SPAWN_H_
#define _KERNEL_SYS_SPAWN_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/syscall/calls/sys_procfile.h>
#include <kernel/syscall/calls/sys_user.h>
#include <kernel/scheduler/process.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/vfs/vfs.h>
#include <kernel/malloc.h>
#include <kernel/util/string.h>

//
// spawn(path, argv or 0, cwd or 0, mask or SYS_SPAWN_INHERIT) -> process id
//
// ABI in syscall.h ("Processes"). syscall_dispatch() has already required
// SYS_SEC_EXEC of the caller.
//
// Everything the call needs from user memory (path, cwd, every argv string)
// is copied into one kernel block first, the descriptor table is let go,
// and only then is the executable loaded: CreateProcessLaunch() reads the
// file through the VFS, which can take a while, and holds no lock of ours.
// The load runs on the caller's thread, so the VFS checks the executable
// against the caller's own file clearance.
//

_Static_assert(SYS_ARG_MAX == PROCESS_ARG_MAX, "SYS_ARG_MAX must equal PROCESS_ARG_MAX");
_Static_assert(SYS_ARG_BYTES == PROCESS_ARG_BYTES, "SYS_ARG_BYTES must equal PROCESS_ARG_BYTES");
_Static_assert(PROCESS_PATH_MAX <= SYS_PATH_MAX, "an image path must fit a syscall path");

// About 5 KB: on the heap, not the 16 KB kernel stack
typedef struct {
    char        path[SYS_PATH_MAX];
    char        cwd[SYS_PATH_MAX];
    const char* argv[SYS_ARG_MAX];
    uint32_t    argc;
    char        strings[SYS_ARG_BYTES];
} SysSpawnRequest;

// Copy a NULL-terminated user array of string pointers
static int32_t sys_spawn_copy_argv(AddressSpace* space, uint32_t user_argv, SysSpawnRequest* q) {
    uint32_t used = 0;
    q->argc = 0;

    for (;;) {
        uint32_t slot = user_argv + q->argc * 4u;
        if (slot < user_argv)
            return SYS_EFAULT;      // Wrapped around

        uint32_t ptr = 0;
        if (!vmm_user_check(space, slot, sizeof(ptr), false) ||
            !vmm_user_copy_from(space, &ptr, slot, sizeof(ptr)))
            return SYS_EFAULT;
        if (ptr == 0)
            return SYS_OK;

        if (q->argc == SYS_ARG_MAX || used == SYS_ARG_BYTES)
            return SYS_E2BIG;

        int32_t length = sys_user_string(space, ptr, q->strings + used, SYS_ARG_BYTES - used);
        if (length == SYS_ENAMETOOLONG)
            return SYS_E2BIG;
        if (length < 0)
            return length;

        q->argv[q->argc++] = q->strings + used;
        used += (uint32_t)length + 1;
    }
}

static int32_t sys_spawn_error(ProcessId code) {
    switch (code) {
        case PROCESS_ERR_INVALID_ARG:    return SYS_EINVAL;
        case PROCESS_ERR_NO_SLOTS:       return SYS_EAGAIN;
        case PROCESS_ERR_NO_MEMORY:      return SYS_ENOMEM;
        case PROCESS_ERR_THREAD:         return SYS_ENOMEM;
        case PROCESS_ERR_NO_PAGING:      return SYS_ENOSYS;
        case PROCESS_ERR_LOAD_FAILED:    return SYS_EIO;
        case PROCESS_ERR_NOT_FOUND:      return SYS_ENOENT;
        case PROCESS_ERR_NOT_EXECUTABLE: return SYS_EACCES;
        case PROCESS_ERR_BAD_IMAGE:      return SYS_EBADFMT;
        case PROCESS_ERR_UNSUPPORTED:    return SYS_EBADFMT;
        case PROCESS_ERR_TOO_BIG:        return SYS_E2BIG;
        default:                         return SYS_EIO;
    }
}

static int32_t sys_spawn(const SyscallArgs* args) {
    AddressSpace* space = process_current_space();
    if (space == NULL)
        return SYS_EFAULT;

    // The child's clearance: the caller's, or a part of it. Never more.
    ProcessSecurity mine = process_current_security();
    ProcessSecurity mask = (ProcessSecurity)args->arg4;
    if (mask == SYS_SPAWN_INHERIT)
        mask = mine;
    else if ((mask & ~SYS_SEC_VALID) != 0)
        return SYS_EINVAL;
    else if ((mask & ~mine) != 0)
        return SYS_EPERM;

    // The child runs at the caller's base priority
    ProcessInfo self;
    ThreadPriority priority = ProcessGetInfo(process_current_id(), &self) ? self.priority
                                                                          : PRIORITY_NORMAL;

    SysSpawnRequest* q = (SysSpawnRequest*)malloc(sizeof(SysSpawnRequest));
    if (q == NULL)
        return SYS_ENOMEM;

    // Path and working directory, both against the caller's working directory
    bool program_dir = false;
    SysProcessFiles* files = sys_files_acquire();
    if (files == NULL) {
        free(q);
        return SYS_ENOMEM;
    }
    int32_t result = sys_user_path(space, files->cwd, args->arg1, q->path);
    if (result == SYS_OK) {
        if (args->arg3 == 0) {
            memcpy(q->cwd, files->cwd, strlen(files->cwd) + 1);
        } else {
            int32_t length = sys_user_string(space, args->arg3, q->cwd, SYS_PATH_MAX);
            if (length < 0)
                result = length;
            else if (length == 0)
                program_dir = true;     // "": where the executable lives
            else if (!vfs_path_normalize(files->cwd, q->cwd, q->cwd, SYS_PATH_MAX))
                result = SYS_ENAMETOOLONG;
        }
    }
    sys_files_release(files);

    if (result == SYS_OK && args->arg2 != 0)
        result = sys_spawn_copy_argv(space, args->arg2, q);
    else
        q->argc = 0;

    // Clear answers for the common failures before the loader runs
    if (result == SYS_OK) {
        FSFileStats st;
        if (strlen(q->path) >= PROCESS_PATH_MAX)
            result = SYS_ENAMETOOLONG;
        else if (!vfs_stat(q->path, &st))
            result = SYS_ENOENT;
        else if (st.type != VFS_TYPE_FILE)
            result = SYS_EISDIR;
        else if (!vfs_check_access(q->path))
            result = SYS_EACCES;
    }

    if (result == SYS_OK) {
        ProcessLaunch launch = {
            .image_path = q->path,
            .cwd        = program_dir ? NULL : q->cwd,
            .priority   = priority,
            .security   = mask,
            .argc       = q->argc,      // 0: argv = { path }
            .argv       = q->argv,
        };
        ProcessId pid = CreateProcessLaunch(&launch);
        result = (pid > 0) ? (int32_t)pid : sys_spawn_error(pid);
    }

    free(q);
    return result;
}

#endif
