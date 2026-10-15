#ifndef _KERNEL_SYS_USER_H_
#define _KERNEL_SYS_USER_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <kernel/syscall/syscall.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/vfs/vfs.h>
#include <kernel/util/string.h>

//
// User memory helpers for the file calls
//
// Nothing here touches a user pointer directly. Every access goes through
// vmm_user_check() + vmm_user_copy_*, so a bad pointer is SYS_EFAULT, never
// a kernel page fault.
//

// Copy a NUL-terminated string from user space into `out` (`size` bytes,
// terminator included). Works one page at a time and stops at the NUL, so a
// string that ends just before an unmapped page is fine.
// Returns its length, SYS_EFAULT or SYS_ENAMETOOLONG.
static int32_t sys_user_string(AddressSpace* space, uint32_t user_addr, char* out, uint32_t size) {
    if (space == NULL || size == 0)
        return SYS_EFAULT;

    uint32_t done = 0;
    while (done < size) {
        uint32_t addr  = user_addr + done;
        uint32_t chunk = PAGE_SIZE - (addr & (PAGE_SIZE - 1));
        if (chunk > size - done)
            chunk = size - done;

        if (!vmm_user_check(space, addr, chunk, false) ||
            !vmm_user_copy_from(space, out + done, addr, chunk))
            return SYS_EFAULT;

        for (uint32_t i = 0; i < chunk; i++) {
            if (out[done + i] == '\0')
                return (int32_t)(done + i);
        }
        done += chunk;
    }

    out[size - 1] = '\0';
    return SYS_ENAMETOOLONG;
}

// Copy a path from user space and make it absolute against `cwd`
static int32_t sys_user_path(AddressSpace* space, const char* cwd, uint32_t user_addr, char out[SYS_PATH_MAX]) {
    int32_t length = sys_user_string(space, user_addr, out, SYS_PATH_MAX);
    if (length < 0)
        return length;
    if (length == 0)
        return SYS_ENOENT;

    // `out` may be both input and output
    if (!vfs_path_normalize(cwd, out, out, SYS_PATH_MAX))
        return SYS_ENAMETOOLONG;
    return SYS_OK;
}

// Copy one name (not a path) from user space: no '/', not "." or ".."
static int32_t sys_user_name(AddressSpace* space, uint32_t user_addr, char out[SYS_NAME_MAX + 1]) {
    int32_t length = sys_user_string(space, user_addr, out, SYS_NAME_MAX + 1);
    if (length < 0)
        return length;
    if (length == 0 || strcmp(out, ".") == 0 || strcmp(out, "..") == 0)
        return SYS_EINVAL;
    for (int32_t i = 0; i < length; i++) {
        if (out[i] == '/')
            return SYS_EINVAL;
    }
    return SYS_OK;
}

// Copy `length` bytes to a user buffer, which must be writable by ring 3
static bool sys_user_put(AddressSpace* space, uint32_t user_addr, const void* src, uint32_t length) {
    return vmm_user_check(space, user_addr, length, true) &&
           vmm_user_copy_to(space, user_addr, src, length);
}

static int32_t sys_stat_to_user(AddressSpace* space, uint32_t user_addr, const FSFileStats* st) {
    SysStat out = {
        .size        = st->size,
        .type        = st->type,
        .permissions = st->permissions,
        .created     = st->created,
        .modified    = st->modified,
    };
    return sys_user_put(space, user_addr, &out, sizeof(out)) ? SYS_OK : SYS_EFAULT;
}

// True if the directory holding `path` (absolute) exists. `path` is cut at
// its last '/' for the check and restored.
static bool sys_parent_is_directory(char* path) {
    char* slash = strrchr(path, '/');
    if (slash == NULL)
        return false;
    if (slash == path)
        return vfs_directory_check("/");

    *slash = '\0';
    bool is_directory = vfs_directory_check(path);
    *slash = '/';
    return is_directory;
}

// "<directory of path>/<name>" into out
static bool sys_sibling_path(const char* path, const char* name, char* out, size_t size) {
    const char* slash = strrchr(path, '/');
    if (slash == NULL)
        return false;

    size_t dir_length  = (size_t)(slash - path);     // 0 for "/item"
    size_t name_length = strlen(name);
    if (dir_length + 1 + name_length + 1 > size)
        return false;

    memcpy(out, path, dir_length);
    out[dir_length] = '/';
    memcpy(out + dir_length + 1, name, name_length + 1);
    return true;
}

#endif
