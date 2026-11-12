#ifndef _KERNEL_SYS_REGISTRY_H_
#define _KERNEL_SYS_REGISTRY_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/malloc.h>
#include <kernel/registry/registry.h>
#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/syscall/calls/sys_user.h>
#include <kernel/syscall/calls/sys_security.h>
#include <kernel/scheduler/process.h>
#include <kernel/util/string.h>

//
// Registry calls: reg_get, reg_keys, reg_values (read), reg_set,
// reg_delete, reg_mkkey, reg_rename (write)
//
// A path names a key: the hive's root key first, then the keys below it,
// '/'-separated ("hkey_root/software/shell/menus/desktop"). Names match
// exactly. Only keys and values marked REGISTRY_PERMISSION_READ read back;
// anything else is SYS_EACCES.
//
// reg_get(path, name, buf, size) -> value length
//
//   The whole of value `name` under the key, into buf. SYS_ERANGE (buf
//   untouched) when size is smaller than the value, so a caller can ask
//   again with more room. A value with no data is 0. Strings are stored
//   with their '\0' (registry_set_string), which counts in the length.
//
// reg_keys(path, index, buf, size) -> name length, 0 past the last key
//
//   The name of the key's index-th subkey (terminated), numbered from 0 in
//   the order the hive keeps them, which is not sorted. Like readdir, a key
//   that changes between calls can skip or repeat an entry.
//
// Both: SYS_ENOENT (no such hive, key or value), SYS_EACCES, SYS_EFAULT,
// SYS_ENAMETOOLONG (path or name too long), SYS_ERANGE (buf too small),
// SYS_ENOMEM.
//
// Data is copied out of the hive under registry_lock() and handed to the
// caller after it is released.
//
// reg_set(path, name, data, size) -> 0
//
//   Create value `name` under the key at `path`, or replace its data, then
//   write the hive back to its file (<home>/sys/hku or hkr), so the change
//   lasts. Keys on the way that don't exist are made (READ | WRITE), which
//   needs the key above them to be writable. The key, and the value if it
//   is there already, must be marked REGISTRY_PERMISSION_WRITE.
//
//   Needs SYS_SEC_FILE_WRITE (syscall table). hkey_user, where per-user
//   settings live (the desktop background, ...), is open to any program
//   holding that; hkey_root, the system's, takes SYS_SEC_ADMIN as well.
//   At most SYS_REG_VALUE_MAX bytes (SYS_EINVAL past that, or for an empty
//   name).
//
//   SYS_ENOENT (no such hive), SYS_EACCES (not writable), SYS_EPERM
//   (hkey_root without admin), SYS_EFAULT, SYS_ENAMETOOLONG (path, name or a
//   key name past REGISTRY_MAX_NAME_LEN - 1), SYS_ENOMEM, SYS_EIO (changed
//   in memory, but the hive file could not be written).
//
// reg_values(path, index, buf, size) -> name length, 0 past the last value
//
//   reg_keys for the key's values: the index-th value's name. The key must
//   be readable; the value itself need not be (reg_get then says EACCES),
//   so an editor can list it.
//
// The calls below change the registry like reg_set: SYS_SEC_FILE_WRITE,
// SYS_SEC_ADMIN as well in hkey_root, and the hive is saved after the
// change (SYS_EIO if it could not be).
//
// reg_delete(path, name) -> 0
//
//   With a name: remove that value from the key at `path`. The key and the
//   value must be writable. With name 0 or "": remove the key at `path`,
//   with every key and value below it. It and the key above it must be
//   writable; a hive's root key can't be removed (SYS_EINVAL). Nothing
//   below the key is checked: whoever may remove a key removes its
//   contents. SYS_ENOENT when there is nothing by that name.
//
// reg_mkkey(path) -> 0
//
//   Make the key at `path`, and the keys on the way (READ | WRITE, each
//   needing the key above it writable). SYS_EEXIST if it is there already.
//
// reg_rename(path, name, new_name) -> 0
//
//   With a name: rename that value of the key at `path`. With name 0 or "":
//   rename the key at `path` itself (not a hive's root key). What is renamed
//   and the key holding it must be writable. SYS_EEXIST when a sibling
//   already has new_name; SYS_EINVAL for an empty new name or one with '/'.
//

// The hive whose root key `path` starts with, or NULL; *rest is what
// follows the root key's name ("" for the root itself). The caller holds
// the registry lock.
static struct RegistryHive* sys_reg_hive(const char* path, const char** rest) {
    const char* slash = strchr(path, '/');
    size_t length = (slash != NULL) ? (size_t)(slash - path) : strlen(path);

    struct RegistryHive* hives[] = { &hkey_root, &hkey_user };
    for (size_t i = 0; i < sizeof(hives) / sizeof(hives[0]); i++) {
        struct RegistryKey* root = hives[i]->root;
        if (root == NULL || strlen(root->name) != length || strncmp(root->name, path, length) != 0)
            continue;
        *rest = (slash != NULL) ? slash + 1 : "";
        return hives[i];
    }
    return NULL;
}

// The key at `path`, or NULL. The caller holds the registry lock.
static struct RegistryKey* sys_reg_open(const char* path) {
    const char* rest;
    struct RegistryHive* hive = sys_reg_hive(path, &rest);
    return (hive != NULL) ? registry_open_key(hive->root, rest, false, 0) : NULL;
}

// Save a hive after a change; `result` stays unless it was SYS_OK and the
// file could not be written. The caller holds the registry lock.
//
// The hive file is the kernel's own, not the caller's: the registry checks
// above already decided whether the change is allowed (hkey_user is open to
// any program with file write clearance, hkey_root takes admin). The file
// itself lives in <home>/sys, which is "write=admin" on disk, so writing it
// with the caller's clearance refused every save made by a program without
// admin (the desktop settings' "could not be saved to disk"). It is written
// with full file clearance for the length of the export, and put back.
static int32_t sys_reg_save(struct RegistryHive* hive, int32_t result) {
    const char* file = registry_hive_path(hive);
    if (file[0] == '\0')
        return result;
    ProcessSecurity old_extra = process_current_set_clearance_extra(SYS_SEC_VALID);
    bool saved = registry_hive_export(hive, file);
    process_current_set_clearance_extra(old_extra);
    if (!saved && result == SYS_OK)
        return SYS_EIO;
    return result;
}

static int32_t sys_reg_get(const SyscallArgs* args) {
    AddressSpace* space = process_current_space();
    uint32_t user_buffer = args->arg3;
    uint32_t size        = args->arg4;

    char path[SYS_PATH_MAX];
    int32_t result = sys_user_string(space, args->arg1, path, sizeof(path));
    if (result < 0)
        return result;
    char name[REGISTRY_MAX_NAME_LEN];
    result = sys_user_string(space, args->arg2, name, sizeof(name));
    if (result < 0)
        return result;

    void* copy = NULL;
    uint32_t length = 0;

    registry_lock();
    struct RegistryKey* key = sys_reg_open(path);
    struct RegistryValue* value = (key != NULL) ? registry_get_value(key, name) : NULL;
    if (key == NULL || value == NULL) {
        result = SYS_ENOENT;
    } else if (!(registry_get_permissions(key) & REGISTRY_PERMISSION_READ) ||
               !(registry_get_permissions(value) & REGISTRY_PERMISSION_READ)) {
        result = SYS_EACCES;
    } else {
        length = (value->data != NULL) ? (uint32_t)value->data_len : 0;
        if (length > size) {
            result = SYS_ERANGE;
        } else if (length > 0) {
            copy = malloc(length);
            if (copy == NULL)
                result = SYS_ENOMEM;
            else
                memcpy(copy, value->data, length);
        }
    }
    registry_unlock();

    if (result < 0)
        return result;
    if (length > 0) {
        bool put = sys_user_put(space, user_buffer, copy, length);
        free(copy);
        if (!put)
            return SYS_EFAULT;
    }
    return (int32_t)length;
}

static int32_t sys_reg_keys(const SyscallArgs* args) {
    AddressSpace* space = process_current_space();
    uint32_t index       = args->arg2;
    uint32_t user_buffer = args->arg3;
    uint32_t size        = args->arg4;

    char path[SYS_PATH_MAX];
    int32_t result = sys_user_string(space, args->arg1, path, sizeof(path));
    if (result < 0)
        return result;

    char name[REGISTRY_MAX_NAME_LEN];
    bool have_name = false;

    registry_lock();
    struct RegistryKey* key = sys_reg_open(path);
    if (key == NULL) {
        result = SYS_ENOENT;
    } else if (!(registry_get_permissions(key) & REGISTRY_PERMISSION_READ)) {
        result = SYS_EACCES;
    } else {
        struct RegistryKey* child = key->child_keys;
        for (uint32_t i = 0; child != NULL && i < index; i++)
            child = child->next;
        if (child == NULL) {
            result = 0;
        } else {
            memcpy(name, child->name, sizeof(name));
            name[sizeof(name) - 1] = '\0';
            have_name = true;
        }
    }
    registry_unlock();

    if (!have_name)
        return result;

    uint32_t length = (uint32_t)strlen(name);
    if (size < length + 1)
        return SYS_ERANGE;
    if (!sys_user_put(space, user_buffer, name, length + 1))
        return SYS_EFAULT;
    return (int32_t)length;
}

// Walk `rest` below `root`, making missing keys where the key above is
// writable. The key, or NULL with *error set. The caller holds the lock.
static struct RegistryKey* sys_reg_open_for_write(struct RegistryKey* root, const char* rest, int32_t* error) {
    const uint16_t perms = REGISTRY_PERMISSION_READ | REGISTRY_PERMISSION_WRITE;
    struct RegistryKey* key = root;
    const char* p = rest;
    char segment[REGISTRY_MAX_NAME_LEN];

    while (*p != '\0') {
        while (*p == '/') p++;
        if (*p == '\0') break;

        size_t n = 0;
        while (p[n] != '\0' && p[n] != '/') n++;
        if (n >= REGISTRY_MAX_NAME_LEN) {
            *error = SYS_ENAMETOOLONG;
            return NULL;
        }
        memcpy(segment, p, n);
        segment[n] = '\0';
        p += n;

        struct RegistryKey* child = registry_get_key(key, segment);
        if (child == NULL) {
            if (!(registry_get_permissions(key) & REGISTRY_PERMISSION_WRITE)) {
                *error = SYS_EACCES;
                return NULL;
            }
            child = registry_create_key(key, segment, perms);
            if (child == NULL) {
                *error = SYS_ENOMEM;
                return NULL;
            }
        }
        key = child;
    }
    return key;
}

static int32_t sys_reg_set(const SyscallArgs* args) {
    AddressSpace* space = process_current_space();
    uint32_t user_data = args->arg3;
    uint32_t size      = args->arg4;

    char path[SYS_PATH_MAX];
    int32_t result = sys_user_string(space, args->arg1, path, sizeof(path));
    if (result < 0)
        return result;
    char name[REGISTRY_MAX_NAME_LEN];
    result = sys_user_string(space, args->arg2, name, sizeof(name));
    if (result < 0)
        return result;
    if (result == 0 || size > SYS_REG_VALUE_MAX)
        return SYS_EINVAL;

    // The data comes in before the lock is taken
    void* data = NULL;
    if (size > 0) {
        data = malloc(size);
        if (data == NULL)
            return SYS_ENOMEM;
        if (!vmm_user_check(space, user_data, size, false) ||
            !vmm_user_copy_from(space, data, user_data, size)) {
            free(data);
            return SYS_EFAULT;
        }
    }

    registry_lock();
    const char* rest = "";
    struct RegistryHive* hive = sys_reg_hive(path, &rest);
    if (hive == NULL) {
        result = SYS_ENOENT;
    } else if (hive != &hkey_user && !sys_security_allows(SYS_SEC_ADMIN)) {
        result = SYS_EPERM;
    } else {
        result = SYS_OK;
        struct RegistryKey* key = sys_reg_open_for_write(hive->root, rest, &result);
        struct RegistryValue* value = (key != NULL) ? registry_get_value(key, name) : NULL;
        if (key == NULL) {
            // result says why
        } else if (!(registry_get_permissions(key) & REGISTRY_PERMISSION_WRITE) ||
                   (value != NULL && !(registry_get_permissions(value) & REGISTRY_PERMISSION_WRITE))) {
            result = SYS_EACCES;
        } else {
            uint16_t perms = (value != NULL) ? registry_get_permissions(value)
                                             : (REGISTRY_PERMISSION_READ | REGISTRY_PERMISSION_WRITE);
            if (!registry_set_value(key, name, perms, data, size))
                result = SYS_ENOMEM;
        }
        // Made keys stay even when setting the value failed: save what is
        // in memory either way, so the file never lags behind it
        result = sys_reg_save(hive, result);
    }
    registry_unlock();

    free(data);
    return result;
}

static int32_t sys_reg_values(const SyscallArgs* args) {
    AddressSpace* space = process_current_space();
    uint32_t index       = args->arg2;
    uint32_t user_buffer = args->arg3;
    uint32_t size        = args->arg4;

    char path[SYS_PATH_MAX];
    int32_t result = sys_user_string(space, args->arg1, path, sizeof(path));
    if (result < 0)
        return result;

    char name[REGISTRY_MAX_NAME_LEN];
    bool have_name = false;

    registry_lock();
    struct RegistryKey* key = sys_reg_open(path);
    if (key == NULL) {
        result = SYS_ENOENT;
    } else if (!(registry_get_permissions(key) & REGISTRY_PERMISSION_READ)) {
        result = SYS_EACCES;
    } else {
        struct RegistryValue* value = key->values;
        for (uint32_t i = 0; value != NULL && i < index; i++)
            value = value->next;
        if (value == NULL) {
            result = 0;
        } else {
            memcpy(name, value->name, sizeof(name));
            name[sizeof(name) - 1] = '\0';
            have_name = true;
        }
    }
    registry_unlock();

    if (!have_name)
        return result;

    uint32_t length = (uint32_t)strlen(name);
    if (size < length + 1)
        return SYS_ERANGE;
    if (!sys_user_put(space, user_buffer, name, length + 1))
        return SYS_EFAULT;
    return (int32_t)length;
}

// An optional name argument: 0 is "" (the key itself). Its length, or an error.
static int32_t sys_reg_optional_name(AddressSpace* space, uint32_t user, char* out, size_t size) {
    out[0] = '\0';
    if (user == 0)
        return 0;
    return sys_user_string(space, user, out, size);
}

// For the calls that change a hive: the hive `path` is in, once the caller
// may write it (*error otherwise), and the rest of the path after its root
// key. The caller holds the registry lock.
static struct RegistryHive* sys_reg_hive_for_write(const char* path, const char** rest, int32_t* error) {
    struct RegistryHive* hive = sys_reg_hive(path, rest);
    if (hive == NULL) {
        *error = SYS_ENOENT;
        return NULL;
    }
    if (hive != &hkey_user && !sys_security_allows(SYS_SEC_ADMIN)) {
        *error = SYS_EPERM;
        return NULL;
    }
    return hive;
}

// Split a key path below the hive root ("a/b/c") into its parent ("a/b")
// and last name ("c"). False if `rest` names the root itself or a name is
// too long. Trailing '/' are ignored.
static bool sys_reg_split(const char* rest, char* parent, size_t parent_size, char* name, size_t name_size) {
    size_t n = strlen(rest);
    while (n > 0 && rest[n - 1] == '/')
        n--;
    if (n == 0)
        return false;
    size_t start = n;
    while (start > 0 && rest[start - 1] != '/')
        start--;
    if (n - start >= name_size || start >= parent_size)
        return false;
    memcpy(name, rest + start, n - start);
    name[n - start] = '\0';
    memcpy(parent, rest, start);
    parent[start] = '\0';
    return true;
}

static int32_t sys_reg_delete(const SyscallArgs* args) {
    AddressSpace* space = process_current_space();

    char path[SYS_PATH_MAX];
    int32_t result = sys_user_string(space, args->arg1, path, sizeof(path));
    if (result < 0)
        return result;
    char name[REGISTRY_MAX_NAME_LEN];
    result = sys_reg_optional_name(space, args->arg2, name, sizeof(name));
    if (result < 0)
        return result;

    registry_lock();
    const char* rest = "";
    result = SYS_OK;
    struct RegistryHive* hive = sys_reg_hive_for_write(path, &rest, &result);
    if (hive != NULL) {
        if (name[0] != '\0') {
            // A value
            struct RegistryKey* key = registry_open_key(hive->root, rest, false, 0);
            struct RegistryValue* value = (key != NULL) ? registry_get_value(key, name) : NULL;
            if (value == NULL)
                result = SYS_ENOENT;
            else if (!(registry_get_permissions(key) & REGISTRY_PERMISSION_WRITE) ||
                     !(registry_get_permissions(value) & REGISTRY_PERMISSION_WRITE))
                result = SYS_EACCES;
            else
                registry_remove_value(key, name);
        } else {
            // A key, and everything below it
            char parent_path[SYS_PATH_MAX];
            char key_name[REGISTRY_MAX_NAME_LEN];
            if (!sys_reg_split(rest, parent_path, sizeof(parent_path), key_name, sizeof(key_name))) {
                result = (rest[0] == '\0' || rest[0] == '/') ? SYS_EINVAL : SYS_ENAMETOOLONG;
            } else {
                struct RegistryKey* parent = registry_open_key(hive->root, parent_path, false, 0);
                struct RegistryKey* key = (parent != NULL) ? registry_get_key(parent, key_name) : NULL;
                if (key == NULL)
                    result = SYS_ENOENT;
                else if (!(registry_get_permissions(parent) & REGISTRY_PERMISSION_WRITE) ||
                         !(registry_get_permissions(key) & REGISTRY_PERMISSION_WRITE))
                    result = SYS_EACCES;
                else
                    registry_remove_key(parent, key_name);
            }
        }
        if (result == SYS_OK)
            result = sys_reg_save(hive, result);
    }
    registry_unlock();
    return result;
}

static int32_t sys_reg_mkkey(const SyscallArgs* args) {
    AddressSpace* space = process_current_space();

    char path[SYS_PATH_MAX];
    int32_t result = sys_user_string(space, args->arg1, path, sizeof(path));
    if (result < 0)
        return result;

    registry_lock();
    const char* rest = "";
    result = SYS_OK;
    struct RegistryHive* hive = sys_reg_hive_for_write(path, &rest, &result);
    if (hive != NULL) {
        if (registry_open_key(hive->root, rest, false, 0) != NULL) {
            result = SYS_EEXIST;
        } else {
            // Keys made on the way stay even if a later one fails: save
            // what is in memory either way, as reg_set does
            sys_reg_open_for_write(hive->root, rest, &result);
            result = sys_reg_save(hive, result);
        }
    }
    registry_unlock();
    return result;
}

static int32_t sys_reg_rename(const SyscallArgs* args) {
    AddressSpace* space = process_current_space();

    char path[SYS_PATH_MAX];
    int32_t result = sys_user_string(space, args->arg1, path, sizeof(path));
    if (result < 0)
        return result;
    char name[REGISTRY_MAX_NAME_LEN];
    result = sys_reg_optional_name(space, args->arg2, name, sizeof(name));
    if (result < 0)
        return result;
    char new_name[REGISTRY_MAX_NAME_LEN];
    result = sys_user_string(space, args->arg3, new_name, sizeof(new_name));
    if (result < 0)
        return result;
    if (result == 0 || strchr(new_name, '/') != NULL)
        return SYS_EINVAL;

    registry_lock();
    const char* rest = "";
    result = SYS_OK;
    struct RegistryHive* hive = sys_reg_hive_for_write(path, &rest, &result);
    if (hive != NULL) {
        if (name[0] != '\0') {
            struct RegistryKey* key = registry_open_key(hive->root, rest, false, 0);
            struct RegistryValue* value = (key != NULL) ? registry_get_value(key, name) : NULL;
            if (value == NULL)
                result = SYS_ENOENT;
            else if (!(registry_get_permissions(key) & REGISTRY_PERMISSION_WRITE) ||
                     !(registry_get_permissions(value) & REGISTRY_PERMISSION_WRITE))
                result = SYS_EACCES;
            else if (strcmp(name, new_name) == 0)
                result = SYS_OK;
            else if (registry_get_value(key, new_name) != NULL)
                result = SYS_EEXIST;
            else
                memcpy(value->name, new_name, sizeof(value->name));
        } else {
            char parent_path[SYS_PATH_MAX];
            char key_name[REGISTRY_MAX_NAME_LEN];
            if (!sys_reg_split(rest, parent_path, sizeof(parent_path), key_name, sizeof(key_name))) {
                result = (rest[0] == '\0' || rest[0] == '/') ? SYS_EINVAL : SYS_ENAMETOOLONG;
            } else {
                struct RegistryKey* parent = registry_open_key(hive->root, parent_path, false, 0);
                struct RegistryKey* key = (parent != NULL) ? registry_get_key(parent, key_name) : NULL;
                if (key == NULL)
                    result = SYS_ENOENT;
                else if (!(registry_get_permissions(parent) & REGISTRY_PERMISSION_WRITE) ||
                         !(registry_get_permissions(key) & REGISTRY_PERMISSION_WRITE))
                    result = SYS_EACCES;
                else if (strcmp(key_name, new_name) == 0)
                    result = SYS_OK;
                else if (registry_get_key(parent, new_name) != NULL)
                    result = SYS_EEXIST;
                else
                    memcpy(key->name, new_name, sizeof(key->name));
            }
        }
        if (result == SYS_OK)
            result = sys_reg_save(hive, result);
    }
    registry_unlock();
    return result;
}

#endif
