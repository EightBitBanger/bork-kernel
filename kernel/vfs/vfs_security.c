
// Security clearance for files and directories on mounted devices.
//
// Every basefs object stores a 32-bit mask (FSBlockHeader.security) of the
// scheduler's SYS_SEC_* bits: the clearance a caller needs to use it. The
// caller's clearance comes from a source the kernel installs
// (process_current_clearance: the process's ProcessSecurity mask, or every
// bit for a kernel thread). Access is allowed when the caller holds every
// bit of the object's mask, so a user program (SYS_SEC_ADMIN clear) cannot
// touch an object marked VFS_SECURITY_ADMIN.
//
// The VFS stays independent of the scheduler: it only knows the callback.
// Until one is installed everyone has VFS_CLEARANCE_ALL.

#include <stdint.h>
#include <stdbool.h>

#include <kernel/knode.h>
#include <kernel/fs/fs.h>

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

// The mask of a live object on `ctx`. False when `address` is not one.
static bool vfs_fs_node_security(struct FSDeviceContext* ctx, uint32_t address, uint32_t* security) {
    if (ctx == NULL || address == FS_NULL || address == 0)
        return false;
    if (!fs_file_check(ctx, address) && !fs_check_directory_valid(ctx, address))
        return false;
    return fs_file_get_security(ctx, address, security);
}

//
// Internal checks (caller holds the VFS lock)
//

bool vfs_fs_node_allowed(struct FSDeviceContext* ctx, uint32_t address) {
    uint32_t clearance = vfs_caller_clearance();
    if (clearance == VFS_CLEARANCE_ALL)
        return true;

    uint32_t required;
    if (!vfs_fs_node_security(ctx, address, &required))
        return true;    // Nothing there to protect; the caller's own checks fail it
    return vfs_clearance_covers(clearance, required);
}

// Everything below directory `dir` is within `clearance`
static bool vfs_subtree_allowed(struct FSDeviceContext* ctx, uint32_t dir, uint32_t clearance, uint32_t depth) {
    if (depth >= VFS_SECURITY_MAX_DEPTH)
        return false;

    uint32_t parent = fs_directory_get_parent(ctx, dir);
    uint32_t count  = fs_directory_get_reference_count(ctx, dir);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t child = fs_directory_get_reference(ctx, dir, i);
        if (child == FS_NULL || child == 0 || child == dir || child == parent)
            continue;

        uint32_t required;
        if (!vfs_fs_node_security(ctx, child, &required))
            continue;
        if (!vfs_clearance_covers(clearance, required))
            return false;

        // Only descend into directories that really live here (as the
        // recursive delete does), so a corrupt link cannot loop
        if (fs_check_directory_valid(ctx, child) && fs_directory_get_parent(ctx, child) == dir &&
            !vfs_subtree_allowed(ctx, child, clearance, depth + 1))
            return false;
    }
    return true;
}

bool vfs_remove_allowed(struct FSDeviceContext* ctx, uint32_t address) {
    uint32_t clearance = vfs_caller_clearance();
    if (clearance == VFS_CLEARANCE_ALL)
        return true;

    uint32_t required;
    if (!vfs_fs_node_security(ctx, address, &required))
        return true;
    if (!vfs_clearance_covers(clearance, required))
        return false;

    // A directory goes with everything inside it
    if (fs_check_directory_valid(ctx, address))
        return vfs_subtree_allowed(ctx, address, clearance, 0);
    return true;
}

bool vfs_path_allowed(const char* path) {
    if (vfs_caller_clearance() == VFS_CLEARANCE_ALL)
        return true;
    if (!vfs_path_fits(path))
        return true;

    uint32_t address = resolve_path_to_address(path);
    if (address == 0xFFFFFFFF || address == 0)
        return true;    // Does not exist

    // Only an open device's addresses are basefs objects; knodes have no mask
    struct FSDeviceContext* ctx = vfs_device_get_context(path);
    if (ctx == NULL)
        return true;
    return vfs_fs_node_allowed(ctx, address);
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
    uint32_t address = resolve_path_to_address(path);
    if (address == 0xFFFFFFFF || address == 0)
        return false;
    struct FSDeviceContext* ctx = vfs_device_get_context(path);
    return vfs_fs_node_security(ctx, address, security);
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

    uint32_t address = resolve_path_to_address(path);
    struct FSDeviceContext* ctx = vfs_device_get_context(path);
    return fs_file_set_security_flag(ctx, address, security);
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
