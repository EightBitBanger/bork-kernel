// Security clearance for files and directories on mounted devices.
//
// Every object on a mounted volume carries a 32-bit mask (FsNodeAttr.security,
// which basefs keeps in FSBlockHeader.security) of the scheduler's SYS_SEC_*
// bits: the clearance a caller needs to use it. A filesystem that cannot
// store masks reports 0 (anyone). The caller's clearance comes from a source
// the kernel installs (process_current_clearance: the process's
// ProcessSecurity mask, or every bit for a kernel thread). Access is allowed when the caller holds every
// bit of the object's mask, so a user program (SYS_SEC_ADMIN clear) cannot
// touch an object marked VFS_SECURITY_ADMIN.
//
// As with Unix search permission, a directory's mask also covers the way
// through it: path resolution (vfs_parse.c) refuses to look anything up in a
// directory the caller is not cleared for, so whatever is inside is out of
// reach whatever its own mask says.
//
// The VFS stays independent of the scheduler: it only knows the callback.
// Until one is installed everyone has VFS_CLEARANCE_ALL.

#include <stdint.h>
#include <stdbool.h>

#include <kernel/knode.h>

#include <kernel/vfs/vfs.h>
#include <kernel/vfs/vfs_internal.h>

#include <kernel/util/string.h>

// Deepest directory nesting a remove is checked through. Deeper trees are
// refused (to a caller without full clearance), never half checked.
#define VFS_SECURITY_MAX_DEPTH   32

static volatile vfs_clearance_fn vfs_clearance_source = NULL;

void vfs_set_clearance_source(vfs_clearance_fn source) {
    vfs_clearance_source = source;  // One pointer write: atomic on i386
}

uint32_t vfs_caller_clearance(void) {
    vfs_clearance_fn source = vfs_clearance_source;
    return (source != NULL) ? source() : VFS_CLEARANCE_ALL;
}

static inline bool vfs_clearance_covers(uint32_t clearance, uint32_t required) {
    return (clearance & required) == required;
}

// The mask of a live object on `mount`. False when `address` is not one.
static bool vfs_fs_node_security(struct FsMount* mount, uint64_t address, uint32_t* security) {
    if (mount == NULL || address == FS_NODE_NULL || address == 0)
        return false;
    FsNodeAttr attr;
    if (!mount->type->get_attr(mount, address, &attr))
        return false;
    *security = attr.security;
    return true;
}

//
// Internal checks (caller holds the VFS lock)
//

bool vfs_fs_node_allowed(struct FsMount* mount, uint64_t address) {
    uint32_t clearance = vfs_caller_clearance();
    if (clearance == VFS_CLEARANCE_ALL)
        return true;

    uint32_t required;
    if (!vfs_fs_node_security(mount, address, &required))
        return true;    // Nothing there to protect; the caller's own checks fail it
    return vfs_clearance_covers(clearance, required);
}

// Everything below directory `dir` is within `clearance`
static bool vfs_subtree_allowed(struct FsMount* mount, uint64_t dir, uint32_t clearance, uint32_t depth) {
    if (depth >= VFS_SECURITY_MAX_DEPTH)
        return false;

    uint64_t parent = mount->type->parent(mount, dir);
    uint32_t count  = mount->type->dir_count(mount, dir);

    for (uint32_t i = 0; i < count; i++) {
        uint64_t child = FS_NODE_NULL;
        if (!mount->type->dir_entry(mount, dir, i, &child, NULL, 0))
            continue;
        if (child == FS_NODE_NULL || child == 0 || child == dir || child == parent)
            continue;

        uint32_t required;
        if (!vfs_fs_node_security(mount, child, &required))
            continue;
        if (!vfs_clearance_covers(clearance, required))
            return false;

        // Only descend into directories that really live here (as the
        // recursive delete does), so a corrupt link cannot loop
        if (vfs_node_is_directory(mount, child) && mount->type->parent(mount, child) == dir &&
            !vfs_subtree_allowed(mount, child, clearance, depth + 1))
            return false;
    }
    return true;
}

bool vfs_remove_allowed(struct FsMount* mount, uint64_t address) {
    uint32_t clearance = vfs_caller_clearance();
    if (clearance == VFS_CLEARANCE_ALL)
        return true;

    uint32_t required;
    if (!vfs_fs_node_security(mount, address, &required))
        return true;
    if (!vfs_clearance_covers(clearance, required))
        return false;

    // A directory goes with everything inside it
    if (vfs_node_is_directory(mount, address))
        return vfs_subtree_allowed(mount, address, clearance, 0);
    return true;
}

// Every directory on the way to `path` is within the caller's clearance,
// device roots included. Shortest prefix first, so the first refused
// directory answers before resolution would just call the rest missing.
static bool vfs_ancestors_allowed(const char* path) {
    char prefix[VFS_PATH_MAX];
    size_t length = strlen(path);   // Fits: checked by the caller

    for (size_t i = 1; i < length; i++) {
        if (path[i] != '/')
            continue;

        memcpy(prefix, path, i);
        prefix[i] = '\0';

        uint64_t address = resolve_path_to_address(prefix);
        if (vfs_address_is_null(address))
            return true;    // Missing from here on: nothing to protect

        // Kernel folders have no mask; only an open device's objects do
        struct FsMount* mount = vfs_device_get_context(prefix);
        if (mount != NULL && !vfs_fs_node_allowed(mount, address))
            return false;
    }
    return true;
}

bool vfs_path_allowed(const char* path) {
    if (vfs_caller_clearance() == VFS_CLEARANCE_ALL)
        return true;
    if (!vfs_path_fits(path))
        return true;
    if (!vfs_ancestors_allowed(path))
        return false;

    uint64_t address = resolve_path_to_address(path);
    if (vfs_address_is_null(address))
        return true;    // Does not exist

    // Only an open device's addresses are filesystem objects; knodes have no mask
    struct FsMount* mount = vfs_device_get_context(path);
    if (mount == NULL)
        return true;
    return vfs_fs_node_allowed(mount, address);
}

bool vfs_parent_allowed(const char* path) {
    if (vfs_caller_clearance() == VFS_CLEARANCE_ALL)
        return true;
    if (!vfs_path_fits(path))
        return true;

    // The parent's own path, so its context is looked up for the parent
    // (a device's root sits in a kernel folder)
    char parent[VFS_PATH_MAX];
    const char* slash = strrchr(path, '/');
    if (slash == NULL)
        return true;
    if (slash == path) {
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        size_t length = (size_t)(slash - path);
        memcpy(parent, path, length);     // Shorter than path: fits
        parent[length] = '\0';
    }
    return vfs_path_allowed(parent);
}

//
// Public API
//

bool vfs_check_access(const char* path) {
    vfs_lock();
    bool result = vfs_path_allowed(path);
    vfs_unlock();
    return result;
}

bool vfs_check_parent_access(const char* path) {
    vfs_lock();
    bool result = vfs_parent_allowed(path);
    vfs_unlock();
    return result;
}

static bool do_get_security(const char* path, uint32_t* security) {
    if (!vfs_path_fits(path) || security == NULL)
        return false;
    uint64_t address = resolve_path_to_address(path);
    if (vfs_address_is_null(address))
        return false;
    struct FsMount* mount = vfs_device_get_context(path);
    return vfs_fs_node_security(mount, address, security);
}

static bool do_set_security(const char* path, uint32_t security) {
    uint32_t old;
    if (!do_get_security(path, &old))
        return false;

    // Nobody grants or strips a bit they do not hold themselves
    uint32_t clearance = vfs_caller_clearance();
    if (!vfs_clearance_covers(clearance, old) || !vfs_clearance_covers(clearance, security))
        return false;
    if (old == security)
        return true;

    uint64_t address = resolve_path_to_address(path);
    struct FsMount* mount = vfs_device_get_context(path);
    if (mount == NULL || mount->type->set_security == NULL)
        return false;   // The filesystem cannot store a mask
    return mount->type->set_security(mount, address, security);
}

bool vfs_get_security(const char* path, uint32_t* security) {
    vfs_lock();
    bool result = do_get_security(path, security);
    vfs_unlock();
    return result;
}

bool vfs_set_security(const char* path, uint32_t security) {
    vfs_lock();
    bool result = do_set_security(path, security);
    vfs_unlock();
    return result;
}
