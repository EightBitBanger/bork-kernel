#include <stdint.h>
#include <stdbool.h>
#include <kernel/malloc.h>

#include <kernel/knode.h>
#include <kernel/fs/fs.h>

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
    uint32_t address = resolve_path_to_address(path);
    if (address == 0xFFFFFFFF || address == 0) 
        return false;
    
    struct FSDeviceContext* ctx = vfs_device_get_context(path);
    if (ctx && (fs_check_directory_valid(ctx, address) || fs_file_check(ctx, address))) {
        uint8_t permissions = 0;
        if (perm & VFS_PERMISSION_EXECUTE)  permissions |= FS_PERMISSION_EXECUTE;
        if (perm & VFS_PERMISSION_READ)     permissions |= FS_PERMISSION_READ;
        if (perm & VFS_PERMISSION_WRITE)    permissions |= FS_PERMISSION_WRITE;
        
        fs_file_set_permissions(ctx, address, permissions);
        return true;
    } else if (kmalloc_is_valid(address)) {
        uint8_t permissions = 0;
        if (perm & VFS_PERMISSION_EXECUTE)  permissions |= KMALLOC_PERMISSION_EXECUTABLE;
        if (perm & VFS_PERMISSION_READ)     permissions |= KMALLOC_PERMISSION_READ;
        if (perm & VFS_PERMISSION_WRITE)    permissions |= KMALLOC_PERMISSION_WRITE;
        
        kmalloc_set_permissions(address, permissions);
        return true;
    }
    return false;
}

static bool do_get_permissions(const char* path, uint8_t* perm) {
    if (path == NULL || path[0] == '\0' || perm == NULL) 
        return false;
    uint32_t address = resolve_path_to_address(path);
    if (address == 0xFFFFFFFF || address == 0) 
        return false;
    *perm = 0;
    
    struct FSDeviceContext* ctx = vfs_device_get_context(path);
    if (ctx && (fs_check_directory_valid(ctx, address) || fs_file_check(ctx, address))) {
        uint8_t permissions = 0;
        fs_file_get_permissions(ctx, address, &permissions);
        
        if (permissions & FS_PERMISSION_EXECUTE) *perm |= VFS_PERMISSION_EXECUTE;
        if (permissions & FS_PERMISSION_READ)    *perm |= VFS_PERMISSION_READ;
        if (permissions & FS_PERMISSION_WRITE)   *perm |= VFS_PERMISSION_WRITE;
        
        return true;
    } else if (kmalloc_is_valid(address)) {
        uint8_t permissions = 0;
        permissions = kmalloc_get_permissions(address);
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
