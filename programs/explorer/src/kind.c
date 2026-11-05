/*
 * What a path is, straight from SYS_STAT. The libc's stat() folds device
 * roots (SYS_TYPE_MOUNT) into plain directories; the explorer needs them
 * apart for the storage icon and the path coloring.
 */

#include <errno.h>
#include <stdint.h>
#include <os/syscall.h>

#include "internal.h"

static int errno_from(int32_t r) {
    switch (r) {
        case SYS_ENOENT:        return ENOENT;
        case SYS_ENOTDIR:       return ENOTDIR;
        case SYS_EPERM:         return EPERM;
        case SYS_EACCES:        return EACCES;
        case SYS_EFAULT:        return EFAULT;
        case SYS_EINVAL:        return EINVAL;
        case SYS_ENAMETOOLONG:  return ENAMETOOLONG;
        case SYS_ENOMEM:        return ENOMEM;
        default:                return EIO;
    }
}

int fs_kind(const char* path, SysStat* out) {
    SysStat s;
    int32_t r = syscall2(SYS_STAT, (uint32_t)(uintptr_t)path, (uint32_t)(uintptr_t)&s);
    if (SYS_IS_ERROR(r)) {
        errno = errno_from(r);
        return -1;
    }
    if (out != NULL)
        *out = s;
    return (int)s.type;
}
