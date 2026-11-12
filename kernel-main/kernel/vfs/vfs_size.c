// Total size of a file or a directory tree (file properties, disk usage).
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

#include <kernel/vfs/vfs.h>
#include <kernel/vfs/vfs_internal.h>

#include <kernel/util/string.h>

#define VFS_SIZE_MAX_DEPTH   32

// File size on a mounted volume, 0 if it cannot be read
static uint64_t size_of_fs_file(struct FsMount* mount, uint64_t node) {
    FsNodeAttr attr;
    return mount->type->get_attr(mount, node, &attr) ? attr.size : 0;
}

// Directory on a mounted volume
static void size_walk_fs(struct FsMount* mount, uint64_t dir, uint32_t depth, VfsDirectorySize* out) {
    if (depth >= VFS_SIZE_MAX_DEPTH) {
        out->complete = false;
        return;
    }

    uint64_t parent = mount->type->parent(mount, dir);
    uint32_t count  = mount->type->dir_count(mount, dir);

    for (uint32_t i = 0; i < count; i++) {
        uint64_t ref = FS_NODE_NULL;
        if (!mount->type->dir_entry(mount, dir, i, &ref, NULL, 0))
            continue;

        // Never follow a link back up (would loop forever)
        if (ref == FS_NODE_NULL || ref == 0 || ref == dir || ref == parent)
            continue;

        uint8_t type = vfs_node_type(mount, ref);
        if (type == FS_NODE_FILE) {
            out->bytes += size_of_fs_file(mount, ref);
            out->files++;
        } else if (type == FS_NODE_DIRECTORY) {
            out->folders++;
            size_walk_fs(mount, ref, depth + 1, out);
        }
    }
}

// Kernel node folder (/, /dev, /mnt, ...)
static void size_walk_knode(void* dir, uint32_t depth, VfsDirectorySize* out) {
    if (depth >= VFS_SIZE_MAX_DEPTH) {
        out->complete = false;
        return;
    }

    uint32_t count = knode_get_reference_count(dir);

    for (uint32_t i = 0; i < count; i++) {
        void* child = knode_get_reference(dir, i);
        if (child == NULL || child == dir)
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

    uint64_t address = resolve_path_to_address(path);
    if (vfs_address_is_null(address))
        return false;

    struct FsMount* mount = vfs_device_get_context(path);

    // On a device (a mount point resolves to the device's root directory)
    if (mount && vfs_node_is_directory(mount, address)) {
        size_walk_fs(mount, address, 0, out);
        return true;
    }
    if (mount && vfs_node_is_file(mount, address)) {
        out->bytes = size_of_fs_file(mount, address);
        out->files = 1;
        return true;
    }

    // Kernel nodes (only when the path is not on an open device)
    void* knode = mount ? NULL : vfs_address_knode(address);
    if (knode != NULL && kmalloc_is_valid(knode)) {
        uint8_t flags = kmalloc_get_flags(knode);

        // Mount whose device is not open (unformatted): nothing to count
        if (flags & KMALLOC_FLAG_MOUNT) {
            out->complete = false;
            return true;
        }

        if (flags & KMALLOC_FLAG_DIRECTORY) {
            size_walk_knode(knode, 0, out);
        } else {
            out->bytes = kmalloc_get_size(knode);
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
