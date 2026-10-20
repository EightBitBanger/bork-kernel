#include <stdint.h>
#include <stdbool.h>
#include <kernel/malloc.h>

#include <kernel/knode.h>

#include <kernel/vfs/vfs.h>
#include <kernel/vfs/vfs_internal.h>

#include <kernel/util/string.h>
#include <kernel/util/tok.h>
#include <kernel/util/list.h>

// Open-file table. Guarded by the VFS lock (see vfs_internal.h).
struct list_node* open_files_head = NULL;
struct list_node* open_files_tail = NULL;
File next_unique_id = 1;

//
// Implementations (caller holds the VFS lock)
//

// True for a knode folder whose references are its entries. A mount knode
// (one whose device is unusable, or path resolution would have entered it)
// holds its FsMount there instead, and a non-folder knode
// has no list at all (knode_get_reference_count returns 0).
static bool vfs_knode_has_children(const void* address) {
    if (!knode_check_is_valid(address))
        return false;
    return (kmalloc_get_flags(address) & KMALLOC_FLAG_MOUNT) == 0;
}

static bool do_directory_check_mounted(const char* path) {
    if (path == NULL || path[0] == '\0') 
        return false;
    uint64_t address = resolve_path_to_address(path);
    if (vfs_address_is_null(address)) 
        return false;
    
    // On an open device the address is a filesystem node: never ask the
    // heap about it (a 64-bit disk address is no knode)
    struct FsMount* mount = vfs_device_get_context(path);
    if (mount) {
        if (!vfs_node_is_directory(mount, address))
            return false;
        return mount->type->parent(mount, address) == FS_NODE_NULL;
    }
    
    void* knode = vfs_address_knode(address);
    return kmalloc_is_valid(knode) &&
           (kmalloc_get_flags(knode) & KMALLOC_FLAG_MOUNT) != 0;
}

static bool do_directory_check(const char* path) {
    if (path == NULL || path[0] == '\0') 
        return false;
    
    if (do_directory_check_mounted(path)) 
        return true;
    
    uint64_t address = resolve_path_to_address(path);
    if (vfs_address_is_null(address)) 
        return false;
    
    struct FsMount* mount = vfs_device_get_context(path);
    if (mount)
        return vfs_node_is_directory(mount, address);
    
    void* knode = vfs_address_knode(address);
    return kmalloc_is_valid(knode) &&
           (kmalloc_get_flags(knode) & (KMALLOC_FLAG_MOUNT | KMALLOC_FLAG_DIRECTORY)) != 0;
}

static uint32_t do_directory_get_item_count(const char* path) {
    if (path == NULL || path[0] == '\0') 
        return 0;
    uint64_t address = resolve_path_to_address(path);
    if (vfs_address_is_null(address)) 
        return 0;
    struct FsMount* mount = vfs_device_get_context(path);
    if (mount && vfs_node_is_directory(mount, address)) {
        if (!vfs_fs_node_allowed(mount, address))
            return 0;   // Listing needs the directory's clearance
        return mount->type->dir_count(mount, address);
    }
    if (mount)
        return 0;   // A file on the device
    void* knode = vfs_address_knode(address);
    if (!vfs_knode_has_children(knode))
        return 0;
    return knode_get_reference_count(knode);
}

static bool do_directory_get_item(const char* path, unsigned int index, char* name_out, size_t name_size) {
    if (name_out == NULL || name_size == 0)
        return false;
    name_out[0] = '\0';
    if (path == NULL || path[0] == '\0') 
        return false;
    uint64_t address = resolve_path_to_address(path);
    if (vfs_address_is_null(address)) 
        return false;
    struct FsMount* mount = vfs_device_get_context(path);
    if (mount && vfs_node_is_directory(mount, address)) {
        if (!vfs_fs_node_allowed(mount, address))
            return false;
        // The name comes straight from the directory entry
        return mount->type->dir_entry(mount, address, index, NULL, name_out, name_size);
    } else {
        void* knode = vfs_address_knode(address);
        if (mount || !vfs_knode_has_children(knode))
            return false;   // A file on the device, or a knode without entries
        void* item_address = knode_get_reference(knode, index);
        if (item_address == NULL)
            return false;
        // knode_get_name has no length parameter: read into a buffer big
        // enough for any knode name, then copy what fits
        char knode_name[VFS_KNODE_NAME_MAX * 2];
        memset(knode_name, 0, sizeof(knode_name));
        knode_get_name(item_address, knode_name);
        knode_name[sizeof(knode_name) - 1] = '\0';
        size_t length = strlen(knode_name);
        if (length >= name_size)
            return false;
        memcpy(name_out, knode_name, length + 1);
        return true;
    }
}

//
// Public API
//

bool vfs_directory_check(const char* path) {
    vfs_lock();
    bool result = do_directory_check(path);
    vfs_unlock();
    return result;
}

bool vfs_directory_check_mounted(const char* path) {
    vfs_lock();
    bool result = do_directory_check_mounted(path);
    vfs_unlock();
    return result;
}

// Note: count and item are separate calls. To iterate a directory that
// another thread may be changing, wrap the loop in vfs_lock()/vfs_unlock().
uint32_t vfs_directory_get_item_count(const char* path) {
    vfs_lock();
    uint32_t result = do_directory_get_item_count(path);
    vfs_unlock();
    return result;
}

bool vfs_directory_get_item(const char* path, unsigned int index, char* name_out, size_t name_size) {
    vfs_lock();
    bool result = do_directory_get_item(path, index, name_out, name_size);
    vfs_unlock();
    return result;
}
