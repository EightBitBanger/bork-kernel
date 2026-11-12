/*
 * Formatting storage devices (os/disk.h) on SYS_FORMAT, unmounting them on
 * SYS_UNMOUNT.
 */

#include <errno.h>
#include <string.h>
#include <os/disk.h>
#include "internal.h"

void disk_format_init(disk_format_t* spec) {
    if (spec == NULL)
        return;
    memset(spec, 0, sizeof(*spec));
    spec->size = sizeof(*spec);
}

static int disk_format_call(const char* device, disk_format_t* spec, uint32_t flags) {
    if (device == NULL || spec == NULL) {
        errno = EINVAL;
        return -1;
    }
    spec->size     = sizeof(*spec);
    spec->flags    = flags;
    spec->reserved = 0;
    int32_t r = syscall2(SYS_FORMAT, (uint32_t)(uintptr_t)device, (uint32_t)(uintptr_t)spec);
    if (SYS_IS_ERROR(r)) {
        errno = __errno_from_sys(r);
        return -1;
    }
    return 0;
}

int disk_format_query(const char* device, disk_format_t* spec) {
    uint32_t quick = (spec != NULL) ? (spec->flags & SYS_FORMAT_QUICK) : 0;
    return disk_format_call(device, spec, SYS_FORMAT_QUERY | quick);
}

int disk_format(const char* device, disk_format_t* spec) {
    uint32_t quick = (spec != NULL) ? (spec->flags & SYS_FORMAT_QUICK) : 0;
    return disk_format_call(device, spec, quick);
}

int disk_unmount(const char* device) {
    if (device == NULL) {
        errno = EINVAL;
        return -1;
    }
    int32_t r = syscall1(SYS_UNMOUNT, (uint32_t)(uintptr_t)device);
    if (SYS_IS_ERROR(r)) {
        errno = __errno_from_sys(r);
        return -1;
    }
    return 0;
}
