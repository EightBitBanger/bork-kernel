#ifndef _KERNEL_SYS_FINFO_H_
#define _KERNEL_SYS_FINFO_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/syscall/calls/sys_procfile.h>
#include <kernel/syscall/calls/sys_user.h>
#include <kernel/syscall/calls/sys_security.h>
#include <kernel/syscall/calls/sys_write.h>
#include <kernel/vfs/vfs.h>
#include <kernel/security/policy.h>
#include <kernel/scheduler/process.h>
#include <kernel/util/string.h>

//
// File details: finfo(path, SysFileInfo*), chmod(path, permissions),
// setpolicy(path, policy), setsecurity(path, mask) (ABI in syscall.h,
// "File details")
//

_Static_assert(sizeof(SysFileInfo) == 48, "SysFileInfo is part of the user ABI");
_Static_assert(SYS_POLICY_NONE == POLICY_NONE && SYS_POLICY_USER == POLICY_USER &&
               SYS_POLICY_ADMIN == POLICY_ADMIN && SYS_POLICY_SYSTEM == POLICY_SYSTEM &&
               SYS_POLICY_NETWORK == POLICY_NETWORK, "SYS_POLICY_* must match POLICY_*");

// ==========================================================================
// VFS adapter. The three calls below are the only VFS functions this file
// uses that the rest of syscall/ doesn't: if your vfs.h names them
// differently, change them here and nothing else.
//
//   vfs_set_permissions(path, uint8_t)       VFS_PERMISSION_* bits (the
//                                            kernel properties window used it)
//   vfs_device_get_used / _capacity(path)    a device root's usage (same);
//                                            _used counts in steps, see vfs.h
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

// Longest SYS_FINFO spends counting a device's used space before it answers
// SYS_FINFO_COUNTING (the caller asks again)
#define SYS_FINFO_COUNT_MS  100

// Called without the VFS lock: the count lets go of it between steps
static inline void sys_finfo_device_usage(const char* path, SysFileInfo* info) {
    uint64_t capacity = vfs_device_get_capacity(path);
    uint64_t used = 0;
    uint32_t percent = 0;
    if (!vfs_device_get_used(path, SYS_FINFO_COUNT_MS, &used, &percent)) {
        info->flags |= SYS_FINFO_COUNTING;
        info->device_percent = percent;
        used = 0;
    }
    info->device_used        = (uint32_t)used;
    info->device_used_hi     = (uint32_t)(used >> 32);
    info->device_capacity    = (uint32_t)capacity;
    info->device_capacity_hi = (uint32_t)(capacity >> 32);
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
    if (!vfs_check_access(path) || !vfs_check_parent_access(path) || sys_write_check(path, false) != SYS_OK)
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
// A program's kind (SYS_FINFO_CONSOLE / _WINDOWED), for a caller that may
// not read the file itself. The same reading as libc's program_kind()
// (programs/libc/src/process.c): the ".os.kind" section's strings, the
// first "console" or "windowed" wins, none means console. Read with full
// clearance for this call only; only the one bit comes out.
//

#define SYS_KIND_SECTION    ".os.kind"
#define SYS_KIND_MAX        512     // Bytes of marks looked at
#define SYS_KIND_SECTIONS   128     // Section headers looked at

typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} SysKindElf;

typedef struct {
    uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize;
} SysKindSection;

static bool sys_kind_read(File f, uint32_t offset, void* buf, uint32_t size) {
    if (vfs_seek(f, offset) != offset)
        return false;
    return vfs_read(f, buf, size) == (int32_t)size;
}

// SYS_FINFO_CONSOLE, SYS_FINFO_WINDOWED, or 0 (not an ELF)
static uint32_t sys_finfo_kind_of(File f) {
    SysKindElf h;
    if (!sys_kind_read(f, 0, &h, sizeof(h)) || memcmp(h.ident, "\x7f" "ELF", 4) != 0 ||
        h.ident[4] != 1 || h.shentsize < sizeof(SysKindSection) || h.shstrndx >= h.shnum)
        return 0;

    SysKindSection names;
    if (!sys_kind_read(f, h.shoff + h.shstrndx * (uint32_t)h.shentsize, &names, sizeof(names)))
        return 0;

    uint32_t count = h.shnum < SYS_KIND_SECTIONS ? h.shnum : SYS_KIND_SECTIONS;
    for (uint32_t i = 1; i < count; i++) {
        SysKindSection sec;
        char name[sizeof(SYS_KIND_SECTION)];
        if (!sys_kind_read(f, h.shoff + i * (uint32_t)h.shentsize, &sec, sizeof(sec)) ||
            sec.name >= names.size ||
            !sys_kind_read(f, names.offset + sec.name, name, sizeof(name)) ||
            memcmp(name, SYS_KIND_SECTION, sizeof(name)) != 0)
            continue;

        char local[SYS_KIND_MAX + 1];
        uint32_t size = sec.size < SYS_KIND_MAX ? sec.size : SYS_KIND_MAX;
        if (!sys_kind_read(f, sec.offset, local, size))
            return SYS_FINFO_CONSOLE;
        local[size] = '\0';
        for (uint32_t at = 0; at < size; at += (uint32_t)strlen(local + at) + 1) {
            if (strcmp(local + at, "console") == 0)
                return SYS_FINFO_CONSOLE;
            if (strcmp(local + at, "windowed") == 0)
                return SYS_FINFO_WINDOWED;
        }
        return SYS_FINFO_CONSOLE;
    }
    return SYS_FINFO_CONSOLE;   // No marks
}

static uint32_t sys_finfo_kind(const char* path) {
    ProcessSecurity old = process_current_set_clearance_extra(SYS_SEC_VALID);
    uint32_t kind = 0;
    File f = vfs_open(path, VFS_OPEN_READ);
    if (f != VFS_INVALID_FILE) {
        kind = sys_finfo_kind_of(f);
        vfs_close(f);
    }
    process_current_set_clearance_extra(old);
    return kind;
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
    uint32_t write_mask = 0;
    if (vfs_get_write_security(path, &write_mask))
        info.write_security = write_mask;
    uint32_t policy = POLICY_NONE;
    if (vfs_get_policy(path, &policy))
        info.policy = policy;
    uint32_t exec_allow = 0, exec_deny = 0;
    if (vfs_get_exec_permissions(path, &exec_allow, &exec_deny)) {
        info.exec_allow = exec_allow;
        info.exec_deny  = exec_deny;
    }
    if (vfs_check_access(path))
        info.flags |= SYS_FINFO_ACCESS;
    if (vfs_check_access(path) && vfs_check_write_access(path))
        info.flags |= SYS_FINFO_WRITE;
    if (sys_chmod_refusal(path) == SYS_OK)
        info.flags |= SYS_FINFO_CHMOD;
    bool device = vfs_directory_check_mounted(path);
    uint8_t permissions = 0;
    bool program = !device && !(info.flags & SYS_FINFO_ACCESS) && !vfs_directory_check(path) &&
                   vfs_get_permissions(path, &permissions) && (permissions & VFS_PERMISSION_EXECUTE);
    vfs_unlock();

    if (program)
        info.flags |= sys_finfo_kind(path);

    if (device) {
        info.flags |= SYS_FINFO_DEVICE;
        sys_finfo_device_usage(path, &info);
    }

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

//
// setpolicy(path, policy), setsecurity(path, mask)
//

// Why the caller may not change the mask of `path` from what it is to
// `new_mask` (*old_mask set), or SYS_OK. The caller holds the VFS lock.
static int32_t sys_secure_refusal(const char* path, uint32_t new_mask, bool keep_mask, uint32_t* old_mask) {
    // As chmod, except clearance for the object itself: an admin may change
    // the mask of one it can't open (vfs_security_change_allowed)
    if (!vfs_exists(path))
        return SYS_ENOENT;
    if (strcmp(path, "/") == 0 || vfs_directory_check_mounted(path))
        return SYS_EBUSY;
    if (!vfs_check_parent_access(path))
        return SYS_EACCES;
    char parent[SYS_PATH_MAX];
    sys_finfo_parent(path, parent);
    uint8_t parent_perm = 0;
    if (!vfs_get_permissions(parent, &parent_perm) || !(parent_perm & VFS_PERMISSION_WRITE))
        return SYS_EACCES;
    uint32_t policy = POLICY_NONE;
    if (!vfs_get_policy(path, &policy) || !vfs_get_security(path, old_mask))
        return SYS_EACCES;      // Not somewhere that keeps a policy and a mask
    if (!vfs_security_change_allowed(*old_mask, keep_mask ? *old_mask : new_mask))
        return SYS_EPERM;
    return SYS_OK;
}

static int32_t sys_setpolicy_locked(SysProcessFiles* t, const SyscallArgs* args) {
    uint32_t policy = args->arg2;
    if (!policy_is_valid(policy))
        return SYS_EINVAL;

    char path[SYS_PATH_MAX];
    int32_t result = sys_user_path(process_current_space(), t->cwd, args->arg1, path);
    if (result < 0)
        return result;

    // Only the group changes: the "allow" bits stay as they are
    uint32_t old_mask = 0;
    vfs_lock();
    result = sys_secure_refusal(path, 0, true, &old_mask);
    if (result == SYS_OK && !vfs_set_policy_only(path, policy))
        result = SYS_EIO;
    vfs_unlock();
    return result;
}

static int32_t sys_setpolicy(const SyscallArgs* args) {
    return sys_files_call(sys_setpolicy_locked, args);
}

static int32_t sys_setsecurity_locked(SysProcessFiles* t, const SyscallArgs* args) {
    uint32_t mask = args->arg2;
    if ((mask & ~SYS_SEC_VALID) != 0)
        return SYS_EINVAL;

    char path[SYS_PATH_MAX];
    int32_t result = sys_user_path(process_current_space(), t->cwd, args->arg1, path);
    if (result < 0)
        return result;

    uint32_t old_mask = 0, policy = POLICY_NONE;
    vfs_lock();
    result = sys_secure_refusal(path, mask, false, &old_mask);
    // In a policy, only the bits it holds can be changed (others already
    // set stay set: picking a policy leaves the mask alone)
    if (result == SYS_OK && vfs_get_policy(path, &policy) && policy_find(policy) != NULL &&
        ((mask ^ old_mask) & ~policy_find(policy)->permissions) != 0)
        result = SYS_EINVAL;
    if (result == SYS_OK && mask != old_mask && !vfs_set_security(path, mask))
        result = SYS_EIO;
    vfs_unlock();
    return result;
}

static int32_t sys_setsecurity(const SyscallArgs* args) {
    return sys_files_call(sys_setsecurity_locked, args);
}

static int32_t sys_setwritesec_locked(SysProcessFiles* t, const SyscallArgs* args) {
    uint32_t mask = args->arg2;
    if ((mask & ~SYS_SEC_VALID) != 0)
        return SYS_EINVAL;

    char path[SYS_PATH_MAX];
    int32_t result = sys_user_path(process_current_space(), t->cwd, args->arg1, path);
    if (result < 0)
        return result;

    uint32_t old_mask = 0, old_write = 0;
    vfs_lock();
    result = sys_secure_refusal(path, 0, true, &old_mask);
    if (result == SYS_OK && !vfs_get_write_security(path, &old_write))
        result = SYS_EACCES;    // A file system that keeps no write mask
    if (result == SYS_OK && mask != old_write && !vfs_set_write_security(path, mask))
        result = SYS_EIO;
    vfs_unlock();
    return result;
}

static int32_t sys_setwritesec(const SyscallArgs* args) {
    return sys_files_call(sys_setwritesec_locked, args);
}

//
// setexecperm(path, allow, deny)
//

static int32_t sys_setexecperm_locked(SysProcessFiles* t, const SyscallArgs* args) {
    uint32_t allow = args->arg2, deny = args->arg3;
    if (((allow | deny) & ~SYS_SEC_PROGRAM_GATED) != 0 || (allow & deny) != 0)
        return SYS_EINVAL;

    char path[SYS_PATH_MAX];
    int32_t result = sys_user_path(process_current_space(), t->cwd, args->arg1, path);
    if (result < 0)
        return result;

    uint32_t old_mask = 0;
    vfs_lock();
    result = sys_secure_refusal(path, 0, true, &old_mask);
    if (result == SYS_OK && vfs_directory_check(path))
        result = SYS_EISDIR;
    if (result == SYS_OK && !vfs_set_exec_permissions(path, allow, deny))
        result = SYS_EACCES;    // A file system that keeps none (FAT)
    vfs_unlock();
    return result;
}

static int32_t sys_setexecperm(const SyscallArgs* args) {
    return sys_files_call(sys_setexecperm_locked, args);
}

#endif
