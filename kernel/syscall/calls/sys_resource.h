#ifndef _KERNEL_SYS_RESOURCE_H_
#define _KERNEL_SYS_RESOURCE_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/syscall/calls/sys_files.h>
#include <kernel/syscall/calls/sys_user.h>
#include <kernel/syscall/calls/sys_gfx.h>
#include <kernel/resources/resource_manager.h>
#include <kernel/resources/classes/image.h>
#include <kernel/vfs/vfs.h>
#include <kernel/scheduler/process.h>
#include <kernel/malloc.h>
#include <kernel/util/string.h>

//
// Resource calls: res_load, res_get, res_info (ABI in syscall.h, "Resources")
//
// A process's resources live in its graphics table (sys_gfx.h), next to its
// surfaces: they are images the interpreter draws from, they count against
// the same pixel budget, and they go when the table does.
//
// res_load never holds a lock across the file read: it resolves the path
// with the descriptor table (for the working directory), drops it, reads
// and decodes the file through the resource manager, and only then takes
// the graphics table to register the result.
//

// Is `name` loaded? Its index, or -1. Caller holds the table mutex.
static int32_t sys_res_find(SysGfxProcess* t, const char* name) {
    for (uint32_t i = 0; i < SYS_RES_MAX; i++) {
        if (t->resources[i] != NULL && strcmp(t->resources[i]->name, name) == 0)
            return (int32_t)i;
    }
    return -1;
}

// Already loaded from `path`: 0; `name` taken by another file: SYS_EEXIST;
// no free handle: SYS_EMFILE; otherwise the free index (>= 0 is returned as
// index + 1 so 0 can mean "already loaded")
static int32_t sys_res_slot(SysGfxProcess* t, const char* name, const char* path) {
    int32_t existing = sys_res_find(t, name);
    if (existing >= 0)
        return (strcmp(t->resources[existing]->path, path) == 0) ? 0 : SYS_EEXIST;
    for (uint32_t i = 0; i < SYS_RES_MAX; i++) {
        if (t->resources[i] == NULL)
            return (int32_t)(i + 1);
    }
    return SYS_EMFILE;
}

//
// res_load(path) -> 0
//

static int32_t sys_res_load(const SyscallArgs* args) {
    AddressSpace* space = process_current_space();
    if (space == NULL)
        return SYS_EFAULT;

    // Absolute path, against the working directory
    char path[SYS_PATH_MAX];
    SysProcessFiles* files = sys_files_acquire();
    if (files == NULL)
        return SYS_ENOMEM;
    int32_t result = sys_user_path(space, files->cwd, args->arg1, path);
    sys_files_release(files);
    if (result < 0)
        return result;

    char name[SYS_RES_NAME_MAX];
    result = sys_gfx_resource_name(path, name);
    if (result < 0)
        return result;

    // Cheap answers first, so a repeat load doesn't read the file again
    SysGfxProcess* t = sys_gfx_acquire();
    if (t == NULL)
        return SYS_ENOMEM;
    result = sys_res_slot(t, name, path);
    sys_gfx_release(t);
    if (result <= 0)
        return result;

    // The file: a sprite resource, through the resource manager
    FSFileStats st;
    if (!vfs_stat(path, &st))
        return SYS_ENOENT;
    if (st.type != VFS_TYPE_FILE)
        return SYS_EISDIR;

    ResourceType type = RESOURCE_TYPE_NONE;
    if (!resource_peek_type(path, &type) || type != RESOURCE_TYPE_SPRITE)
        return SYS_EBADFMT;

    struct Sprite* sprite = resource_sprite_load(path);
    if (sprite == NULL)
        return SYS_EBADFMT;     // Damaged payload (or the kernel heap is out)

    uint32_t bytes = sys_gfx_sprite_bytes(sprite);
    if (bytes == 0) {
        resource_free(RESOURCE_TYPE_SPRITE, sprite);
        return SYS_EBADFMT;     // Too large to draw
    }

    SysGfxResource* r = (SysGfxResource*)malloc(sizeof(SysGfxResource));
    char* saved_path = (char*)malloc(strlen(path) + 1);
    if (r == NULL || saved_path == NULL) {
        free(r);
        free(saved_path);
        resource_free(RESOURCE_TYPE_SPRITE, sprite);
        return SYS_ENOMEM;
    }
    memset(r, 0, sizeof(SysGfxResource));
    memcpy(saved_path, path, strlen(path) + 1);

    // Register. Checked again: another thread of this process may have
    // loaded something in the meantime.
    t = sys_gfx_acquire();
    if (t == NULL) {
        result = SYS_ENOMEM;
    } else {
        result = sys_res_slot(t, name, path);
        if (result > 0) {
            uint32_t index = (uint32_t)result - 1;
            if (!sys_gfx_charge(t, bytes)) {
                result = SYS_ENOMEM;
            } else {
                result = sys_gfx_resource_from_sprite(r, sprite);
                if (result < 0) {
                    sys_gfx_uncharge(t, bytes);
                } else {
                    memcpy(r->name, name, sizeof(name));
                    r->path = saved_path;
                    saved_path = NULL;
                    t->resources[index] = r;
                    r = NULL;
                    result = SYS_OK;
                }
            }
        }
        sys_gfx_release(t);
    }

    free(r);
    free(saved_path);
    resource_free(RESOURCE_TYPE_SPRITE, sprite);
    return result;
}

//
// res_get(name) -> handle
//

static int32_t sys_res_get_locked(SysGfxProcess* t, const SyscallArgs* args) {
    char name[SYS_RES_NAME_MAX];
    int32_t length = sys_user_string(t->space, args->arg1, name, sizeof(name));
    if (length < 0)
        return length;
    if (length == 0)
        return SYS_EINVAL;

    int32_t index = sys_res_find(t, name);
    return (index < 0) ? SYS_ENOENT : index + 1;
}

static int32_t sys_res_get(const SyscallArgs* args) {
    return sys_gfx_call(sys_res_get_locked, args);
}

//
// res_info(handle, SysResInfo*)
//

static int32_t sys_res_info_locked(SysGfxProcess* t, const SyscallArgs* args) {
    const SysGfxResource* r = sys_gfx_get_resource(t, args->arg1);
    if (r == NULL)
        return SYS_EBADF;

    SysResInfo info = {
        .type     = r->type,
        .width    = (uint32_t)r->surface.width,
        .height   = (uint32_t)r->surface.height,
        .reserved = 0,
    };
    return sys_user_put(t->space, args->arg2, &info, sizeof(info)) ? SYS_OK : SYS_EFAULT;
}

static int32_t sys_res_info(const SyscallArgs* args) {
    return sys_gfx_call(sys_res_info_locked, args);
}

#endif
