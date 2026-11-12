#include <stdint.h>
#include <stdbool.h>

#include <kernel/knode.h>

#include <kernel/vfs/vfs.h>
#include <kernel/vfs/vfs_internal.h>

#include <kernel/security/policy.h>

#include <kernel/util/string.h>

//
// Group policies (kernel/security/policy.h)
//
// Only objects on a mounted device whose filesystem stores a policy
// (basefs) have one, files and directories alike. Kernel nodes have none.
//
// Implementations (caller holds the VFS lock)
//

// The object at `path` on a mounted device
static bool resolve_device_object(const char* path, struct FsMount** out_mount, uint64_t* out_address) {
    if (path == NULL || path[0] == '\0' || !vfs_path_fits(path))
        return false;
    uint64_t address = resolve_path_to_address(path);
    if (vfs_address_is_null(address))
        return false;

    struct FsMount* mount = vfs_device_get_context(path);
    if (mount == NULL)
        return false;
    if (!vfs_node_is_file(mount, address) && !vfs_node_is_directory(mount, address))
        return false;

    *out_mount = mount;
    *out_address = address;
    return true;
}

static bool do_get_policy(const char* path, uint32_t* policy) {
    if (policy == NULL)
        return false;
    struct FsMount* mount;
    uint64_t address;
    if (!resolve_device_object(path, &mount, &address))
        return false;
    FsNodeAttr attr;
    if (!mount->type->get_attr(mount, address, &attr))
        return false;
    *policy = attr.policy;
    return true;
}

static bool do_set_policy(const char* path, uint32_t policy, bool keep_mask) {
    if (!policy_is_valid(policy))
        return false;

    struct FsMount* mount;
    uint64_t address;
    if (!resolve_device_object(path, &mount, &address))
        return false;
    if (mount->type->set_policy == NULL || mount->type->set_security == NULL)
        return false;   // The filesystem cannot store one

    // Cleared for the way to it; the object itself is up to
    // vfs_security_change_allowed (an admin may change one it can't open)
    if (!vfs_parent_allowed(path))
        return false;

    FsNodeAttr attr;
    if (!mount->type->get_attr(mount, address, &attr))
        return false;

    // POLICY_NONE takes the object out of its group and leaves its mask as
    // it is. Any other policy keeps the bits of the mask the policy holds,
    // clears the ones it doesn't (moving to a smaller policy takes them
    // away) and adds the policy's clearance. Under the same rule as
    // vfs_set_security (vfs_security_change_allowed).
    // keep_mask (SYS_SETPOLICY): only the group changes.
    uint32_t security = attr.security;
    if (policy != POLICY_NONE && !keep_mask)
        security = (attr.security & policy_find(policy)->permissions) | policy_clearance(policy);
    if (!vfs_security_change_allowed(attr.security, security))
        return false;

    if (security != attr.security && !mount->type->set_security(mount, address, security))
        return false;
    if (policy != attr.policy && !mount->type->set_policy(mount, address, policy))
        return false;
    return true;
}

//
// Public API
//

bool vfs_get_policy(const char* path, uint32_t* policy) {
    vfs_lock();
    bool result = do_get_policy(path, policy);
    vfs_unlock();
    return result;
}

bool vfs_set_policy(const char* path, uint32_t policy) {
    vfs_lock();
    bool result = do_set_policy(path, policy, false);
    vfs_unlock();
    return result;
}

bool vfs_set_policy_only(const char* path, uint32_t policy) {
    vfs_lock();
    bool result = do_set_policy(path, policy, true);
    vfs_unlock();
    return result;
}
