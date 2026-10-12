#include <stdint.h>
#include <stdbool.h>
#include <kernel/malloc.h>

#include <kernel/knode.h>
#include <kernel/fs/fs.h>

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
// holds its cache window and context there instead, and a non-folder knode
// has no list at all (knode_get_reference_count returns KMALLOC_NULL).
static bool vfs_knode_has_children(uint32_t address) {
    if (!knode_check_is_valid(address))
        return false;
    return (kmalloc_get_flags(address) & KMALLOC_FLAG_MOUNT) == 0;
}

static bool do_directory_check_mounted(const char* path) {
    if (path == NULL || path[0] == '\0') 
        return false;
    uint32_t address = resolve_path_to_address(path);
    if (address == KNODE_NULL || address == FS_NULL) 
        return false;
    if (kmalloc_is_valid(address)) {
        uint8_t k_flags = kmalloc_get_flags(address);
        if (k_flags & KMALLOC_FLAG_MOUNT) {
            return true;
        }
    }
    
    struct FSDeviceContext* ctx = vfs_device_get_context(path);
    if (ctx && fs_check_directory_valid(ctx, address)) {
        uint32_t parent = fs_directory_get_parent(ctx, address);
        if (parent == address || parent == FS_NULL) { 
            return true;
        }
    }
    
    return false;
}

static bool do_directory_check(const char* path) {
    if (path == NULL || path[0] == '\0') 
        return false;
    
    if (do_directory_check_mounted(path)) 
        return true;
    
    uint32_t address = resolve_path_to_address(path);
    if (address == KNODE_NULL || address == FS_NULL) 
        return false;
    
    if (kmalloc_is_valid(address)) {
        uint8_t k_flags = kmalloc_get_flags(address);
        if (k_flags & (KMALLOC_FLAG_MOUNT | KMALLOC_FLAG_DIRECTORY)) 
            return true;
    }
    
    struct FSDeviceContext* ctx = vfs_device_get_context(path);
    if (ctx && fs_check_directory_valid(ctx, address)) 
        return true;
    
    return false;
}

static uint32_t do_directory_get_item_count(const char* path) {
    if (path == NULL || path[0] == '\0') 
        return 0;
    uint32_t address = resolve_path_to_address(path);
    if (address == KNODE_NULL || address == FS_NULL) 
        return 0;
    struct FSDeviceContext* ctx = vfs_device_get_context(path);
    if (ctx && fs_check_directory_valid(ctx, address))
        return fs_directory_get_reference_count(ctx, address);
    if (!vfs_knode_has_children(address))
        return 0;
    return knode_get_reference_count(address);
}

static bool do_directory_get_item(const char* path, unsigned int index, char* name_out, size_t name_size) {
    if (name_out == NULL || name_size == 0)
        return false;
    name_out[0] = '\0';
    if (path == NULL || path[0] == '\0') 
        return false;
    uint32_t address = resolve_path_to_address(path);
    if (address == KNODE_NULL || address == FS_NULL) 
        return false;
    struct FSDeviceContext* ctx = vfs_device_get_context(path);
    if (ctx && fs_check_directory_valid(ctx, address)) {
        // The name comes straight from the directory entry
        return fs_directory_get_entry(ctx, address, index, NULL, name_out, name_size, NULL);
    } else {
        if (!vfs_knode_has_children(address))
            return false;
        uint32_t item_address = knode_get_reference(address, index);
        if (item_address == KNODE_NULL || item_address == 0)
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


