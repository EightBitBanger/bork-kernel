#ifndef _KERNEL_SYS_WRITE_H_
#define _KERNEL_SYS_WRITE_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/syscall/syscall.h>
#include <kernel/vfs/vfs.h>

//
// Write clearance for the path calls
//
// An object's write requirement is its security mask plus its extra write
// mask (vfs.h, "Security clearance"): a folder can be readable by every
// program and changeable only by an administrator. The VFS checks it in
// every operation by itself; the calls ask first so a refusal comes back
// as SYS_EACCES rather than as a general failure.
//
// `entry`: the call also changes the directory holding `path` (create,
// remove, rename, move), so it needs that directory's write clearance too.
// A path that doesn't exist yet is only checked through its directory.
//

static inline int32_t sys_write_check(const char* path, bool entry) {
    if (!vfs_check_write_access(path))
        return SYS_EACCES;
    if (entry && !vfs_check_parent_write_access(path))
        return SYS_EACCES;
    return SYS_OK;
}

#endif
