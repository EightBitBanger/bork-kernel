// Total size of a file or a directory tree (used by the Properties window).
//
// The walk runs on node addresses, not paths, so no item is resolved by
// name. The VFS lock is held for the whole walk, so the totals describe one
// consistent state of the tree.
//
// Recursion keeps nothing large on the stack (no path buffers), and the
// depth is capped. Items past the cap, and storage devices mounted inside a
// kernel folder, are not counted; `complete` is then false.

#include <stdint.h>
#include <stdbool.h>
#include <kernel/malloc.h>

#include <kernel/knode.h>
#include <kernel/fs/fs.h>

#include <kernel/vfs/vfs.h>
#include <kernel/vfs/vfs_internal.h>

#include <kernel/util/string.h>

#define VFS_SIZE_MAX_DEPTH   32

// Directory on a mounted basefs device
static void size_walk_fs(struct FSDeviceContext* ctx, uint32_t dir, uint32_t depth, VfsDirectorySize* out) {
    if (depth >= VFS_SIZE_MAX_DEPTH) {
        out->complete = false;
        return;
    }

    uint32_t parent = fs_directory_get_parent(ctx, dir);
    uint32_t count  = fs_directory_get_reference_count(ctx, dir);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t ref = fs_directory_get_reference(ctx, dir, i);

        // Never follow a link back up (would loop forever)
        if (ref == FS_NULL || ref == 0 || ref == dir || ref == parent)
            continue;

        if (fs_file_check(ctx, ref)) {
            out->bytes += fs_file_get_size(ctx, ref);
            out->files++;
        } else if (fs_check_directory_valid(ctx, ref)) {
            out->folders++;
            size_walk_fs(ctx, ref, depth + 1, out);
        }
    }
}

// Kernel node folder (/, /dev, /mnt, ...)
static void size_walk_knode(uint32_t dir, uint32_t depth, VfsDirectorySize* out) {
    if (depth >= VFS_SIZE_MAX_DEPTH) {
        out->complete = false;
        return;
    }

    uint32_t count = knode_get_reference_count(dir);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t child = knode_get_reference(dir, i);
        if (child == KNODE_NULL || child == 0 || child == dir)
            continue;
        if (!kmalloc_is_valid(child))
            continue;

        uint8_t flags = kmalloc_get_flags(child);

        // A storage device mounted here is its own volume: not counted
        if (flags & KMALLOC_FLAG_MOUNT) {
            out->complete = false;
            continue;
        }

        if (flags & KMALLOC_FLAG_DIRECTORY) {
            out->folders++;
            size_walk_knode(child, depth + 1, out);
        } else {
            out->bytes += kmalloc_get_size(child);
            out->files++;
        }
    }
}

// Caller holds the VFS lock
static bool do_directory_get_size(const char* path, VfsDirectorySize* out) {
    memset(out, 0, sizeof(VfsDirectorySize));
    out->complete = true;

    if (path == NULL || path[0] == '\0')
        return false;

    uint32_t address = resolve_path_to_address(path);
    if (address == 0xFFFFFFFF || address == 0)
        return false;

    struct FSDeviceContext* ctx = vfs_device_get_context(path);

    // On a device (a mount point resolves to the device's root directory)
    if (ctx && fs_check_directory_valid(ctx, address)) {
        size_walk_fs(ctx, address, 0, out);
        return true;
    }
    if (ctx && fs_file_check(ctx, address)) {
        out->bytes = fs_file_get_size(ctx, address);
        out->files = 1;
        return true;
    }

    // Kernel nodes
    if (kmalloc_is_valid(address)) {
        uint8_t flags = kmalloc_get_flags(address);

        // Mount whose device is not open (unformatted): nothing to count
        if (flags & KMALLOC_FLAG_MOUNT) {
            out->complete = false;
            return true;
        }

        if (flags & KMALLOC_FLAG_DIRECTORY) {
            size_walk_knode(address, 0, out);
        } else {
            out->bytes = kmalloc_get_size(address);
            out->files = 1;
        }
        return true;
    }

    return false;
}

//
// Public API
//

bool vfs_directory_get_size(const char* path, VfsDirectorySize* out) {
    if (out == NULL)
        return false;

    vfs_lock();
    bool result = do_directory_get_size(path, out);
    vfs_unlock();
    return result;
}
