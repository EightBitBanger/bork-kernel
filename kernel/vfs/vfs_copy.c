#include <stdint.h>
#include <stdbool.h>

#include <kernel/malloc.h>
#include <kernel/vfs/vfs.h>
#include <kernel/util/string.h>

// Copy files and directory trees
//
// Built on the public vfs_* calls, so every step takes the VFS lock on its
// own. The lock is deliberately NOT held across the whole copy: a large tree
// would otherwise freeze every other VFS user for the full duration.
//
// Stack use matters here: thread stacks are 16 KB and every vfs_* call
// already puts a VFS_PATH_MAX path scratch on the stack, so the per-level
// path and name buffers of the recursive directory copy live on the heap
// and the depth is capped.

#define VFS_COPY_CHUNK       4096
#define VFS_COPY_MAX_DEPTH   8
#define VFS_COPY_PATH_MAX    VFS_PATH_MAX

// True when `path` is `base` itself or somewhere below it
static bool copy_path_is_within(const char* path, const char* base) {
    size_t base_len = strlen(base);
    while (base_len > 1 && base[base_len - 1] == '/') base_len--;   // "/a/" == "/a"

    if (strncmp(path, base, base_len) != 0) return false;
    return path[base_len] == '\0' || path[base_len] == '/';
}

// "<dir>/<name>" into out. False if it does not fit.
static bool copy_join(char* out, size_t size, const char* dir, const char* name) {
    size_t dir_len  = strlen(dir);
    size_t name_len = strlen(name);
    bool   slash    = (dir_len == 0 || dir[dir_len - 1] != '/');

    if (dir_len + (slash ? 1 : 0) + name_len + 1 > size) return false;

    memcpy(out, dir, dir_len);
    if (slash) out[dir_len++] = '/';
    memcpy(&out[dir_len], name, name_len + 1);
    return true;
}

// The copy is at least as protected as the original and as its new directory
static void copy_security(const char* src, const char* dest) {
    uint32_t from = 0, to = 0;
    if (!vfs_get_security(src, &from) || from == VFS_SECURITY_NONE)
        return;
    vfs_get_security(dest, &to);
    if ((to | from) != to)
        vfs_set_security(dest, to | from);
}

static bool copy_file(const char* src, const char* dest) {
    File in = vfs_open(src, VFS_OPEN_READ);
    if (in == VFS_INVALID_FILE) return false;

    uint64_t size = vfs_get_size(in);

    // Create the destination, then size it BEFORE writing: writes do not
    // reliably extend a file past the space it already owns
    File out = vfs_open(dest, VFS_OPEN_CREATE | VFS_OPEN_READ | VFS_OPEN_WRITE);
    if (out == VFS_INVALID_FILE) {
        vfs_close(in);
        return false;
    }
    vfs_close(out);

    if (!vfs_truncate(dest, size)) {
        vfs_close(in);
        return false;
    }

    bool ok = true;

    if (size > 0) {
        uint8_t* buffer = (uint8_t*)malloc(VFS_COPY_CHUNK);
        out = vfs_open(dest, VFS_OPEN_READ | VFS_OPEN_WRITE);

        if (buffer == NULL || out == VFS_INVALID_FILE) {
            ok = false;
        } else {
            vfs_seek(in, 0);
            vfs_seek(out, 0);

            uint64_t remaining = size;
            while (remaining > 0) {
                uint32_t chunk = (remaining < VFS_COPY_CHUNK) ? (uint32_t)remaining : VFS_COPY_CHUNK;

                int32_t got = vfs_read(in, buffer, chunk);
                if (got <= 0) { ok = false; break; }   // Unreadable source (permissions)

                if (vfs_write(out, buffer, (uint32_t)got) != got) { ok = false; break; }

                remaining -= (uint32_t)got;
            }
        }

        if (out != VFS_INVALID_FILE) vfs_close(out);
        if (buffer != NULL) free(buffer);
    }

    vfs_close(in);

    // Same permissions as the original
    if (ok) {
        uint8_t perm = 0;
        if (vfs_get_permissions(src, &perm))
            vfs_set_permissions(dest, perm);
        copy_security(src, dest);
    }

    return ok;
}

static bool copy_any(const char* src, const char* dest, uint32_t depth);

static bool copy_directory(const char* src, const char* dest, uint32_t depth) {
    if (depth >= VFS_COPY_MAX_DEPTH) return false;

    if (!vfs_mkdir(dest) || !vfs_directory_check(dest)) return false;

    char* child_src  = (char*)malloc(VFS_COPY_PATH_MAX);
    char* child_dest = (char*)malloc(VFS_COPY_PATH_MAX);
    char* name       = (char*)malloc(VFS_NAME_MAX + 1);
    if (child_src == NULL || child_dest == NULL || name == NULL) {
        if (child_src)  free(child_src);
        if (child_dest) free(child_dest);
        if (name)       free(name);
        return false;
    }

    bool ok = true;
    uint32_t count = vfs_directory_get_item_count(src);

    for (uint32_t i = 0; i < count && ok; i++) {
        if (!vfs_directory_get_item(src, i, name, VFS_NAME_MAX + 1)) continue;
        if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        if (!copy_join(child_src,  VFS_COPY_PATH_MAX, src,  name) ||
            !copy_join(child_dest, VFS_COPY_PATH_MAX, dest, name)) {
            ok = false;
            break;
        }

        ok = copy_any(child_src, child_dest, depth + 1);
    }

    free(child_src);
    free(child_dest);
    free(name);

    // Same permissions as the original
    if (ok) {
        uint8_t perm = 0;
        if (vfs_get_permissions(src, &perm))
            vfs_set_permissions(dest, perm);
        copy_security(src, dest);
    }

    return ok;
}

static bool copy_any(const char* src, const char* dest, uint32_t depth) {
    // Never descend into another device mounted somewhere inside the tree
    if (vfs_directory_check_mounted(src))
        return false;
    
    // Needs the source's clearance (a directory the caller may not list
    // would otherwise come out as an empty copy)
    if (!vfs_check_access(src))
        return false;
    
    if (vfs_directory_check(src))
        return copy_directory(src, dest, depth);
    return copy_file(src, dest);
}

bool vfs_copy(const char* src, const char* dest) {
    if (src == NULL || dest == NULL || src[0] == '\0' || dest[0] == '\0')
        return false;

    if (!vfs_exists(src) || vfs_exists(dest))
        return false;

    // A whole storage device is not something to duplicate into a directory
    if (vfs_directory_check_mounted(src))
        return false;

    // Copying a directory into itself would never finish
    if (copy_path_is_within(dest, src))
        return false;

    bool ok = copy_any(src, dest, 0);

    // Don't leave a half-copied file or tree behind. `dest` did not exist
    // before, so everything there now came from this call.
    if (!ok && vfs_exists(dest))
        vfs_remove(dest);

    return ok;
}
