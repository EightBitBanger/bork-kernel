#include <stdint.h>
#include <stdbool.h>
#include <kernel/malloc.h>

#include <kernel/knode.h>

#include <kernel/vfs/vfs_internal.h>

#include <kernel/util/string.h>
#include <kernel/util/tok.h>
#include <kernel/util/list.h>

//
// Implementations (caller holds the VFS lock)
//

static bool do_set_permissions(const char* path, uint8_t perm) {
    if (path == NULL || path[0] == '\0') 
        return false;
    uint64_t address = resolve_path_to_address(path);
    if (vfs_address_is_null(address)) 
        return false;
    
    struct FsMount* mount = vfs_device_get_context(path);
    if (mount && vfs_node_type(mount, address) != FS_NODE_NONE) {
        if (!vfs_fs_node_allowed(mount, address))
            return false;
        uint8_t permissions = perm & (VFS_PERMISSION_EXECUTE | VFS_PERMISSION_READ | VFS_PERMISSION_WRITE);
        mount->type->set_permissions(mount, address, permissions);
        return true;
    }
    
    void* knode = mount ? NULL : vfs_address_knode(address);
    if (kmalloc_is_valid(knode)) {
        uint8_t permissions = 0;
        if (perm & VFS_PERMISSION_EXECUTE)  permissions |= KMALLOC_PERMISSION_EXECUTABLE;
        if (perm & VFS_PERMISSION_READ)     permissions |= KMALLOC_PERMISSION_READ;
        if (perm & VFS_PERMISSION_WRITE)    permissions |= KMALLOC_PERMISSION_WRITE;
        
        kmalloc_set_permissions(knode, permissions);
        return true;
    }
    return false;
}

static bool do_get_permissions(const char* path, uint8_t* perm) {
    if (path == NULL || path[0] == '\0' || perm == NULL) 
        return false;
    uint64_t address = resolve_path_to_address(path);
    if (vfs_address_is_null(address)) 
        return false;
    *perm = 0;
    
    struct FsMount* mount = vfs_device_get_context(path);
    if (mount && vfs_node_type(mount, address) != FS_NODE_NONE) {
        *perm = vfs_node_permissions(mount, address);
        return true;
    }
    
    void* knode = mount ? NULL : vfs_address_knode(address);
    if (kmalloc_is_valid(knode)) {
        uint8_t permissions = 0;
        permissions = kmalloc_get_permissions(knode);
        if (permissions & KMALLOC_PERMISSION_EXECUTABLE) *perm |= VFS_PERMISSION_EXECUTE;
        if (permissions & KMALLOC_PERMISSION_READ)       *perm |= VFS_PERMISSION_READ;
        if (permissions & KMALLOC_PERMISSION_WRITE)      *perm |= VFS_PERMISSION_WRITE;
        return true;
    }
    return false;
}

//
// Public API
//

bool vfs_set_permissions(const char* path, uint8_t perm) {
    vfs_lock();
    bool result = do_set_permissions(path, perm);
    vfs_unlock();
    return result;
}

bool vfs_get_permissions(const char* path, uint8_t* perm) {
    vfs_lock();
    bool result = do_get_permissions(path, perm);
    vfs_unlock();
    return result;
}
