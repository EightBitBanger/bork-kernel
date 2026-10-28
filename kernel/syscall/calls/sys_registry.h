#ifndef _KERNEL_SYS_REGISTRY_H_
#define _KERNEL_SYS_REGISTRY_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/malloc.h>
#include <kernel/registry/registry.h>
#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/syscall/calls/sys_user.h>
#include <kernel/scheduler/process.h>
#include <kernel/util/string.h>

//
// Registry calls (read only): reg_get, reg_keys
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

// The key at `path`, or NULL. The caller holds the registry lock.
static struct RegistryKey* sys_reg_open(const char* path) {
    const char* slash = strchr(path, '/');
    size_t length = (slash != NULL) ? (size_t)(slash - path) : strlen(path);

    struct RegistryHive* hives[] = { &hkey_root, &hkey_user };
    for (size_t i = 0; i < sizeof(hives) / sizeof(hives[0]); i++) {
        struct RegistryKey* root = hives[i]->root;
        if (root == NULL || strlen(root->name) != length || strncmp(root->name, path, length) != 0)
            continue;
        return registry_open_key(root, (slash != NULL) ? slash + 1 : "", false, 0);
    }
    return NULL;
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

#endif
