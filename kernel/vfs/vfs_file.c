#include <stdint.h>
#include <stdbool.h>
#include <kernel/malloc.h>

#include <kernel/knode.h>
#include <kernel/fs/fs.h>

#include <kernel/vfs/vfs_internal.h>

#include <kernel/util/string.h>
#include <kernel/util/tok.h>
#include <kernel/util/list.h>

// Name validation (vfs_name_is_valid) and the VFS_NAME_MAX == FS_NAME_MAX
// check live in vfs_parse.c, shared with path resolution.

// True if an open file sits at `address` or anywhere below it on `ctx`.
// Freeing such an object would leave the handle pointing at freed blocks.
// Walks each open file's parent chain (every header records its parent).
static bool vfs_fs_node_in_use(struct FSDeviceContext* ctx, uint32_t address) {
    for (struct list_node* n = open_files_head; n != NULL; n = n->next) {
        OpenFileDescriptor* desc = (OpenFileDescriptor*)n->data;
        if (desc == NULL || !desc->in_file_system || desc->ctx != ctx)
            continue;
        uint32_t node  = desc->address;
        uint32_t guard = 1024;      // Far deeper than any real tree
        while (node != FS_NULL && node != 0 && guard-- > 0) {
            if (node == address)
                return true;
            node = fs_file_get_parent(ctx, node);
        }
    }
    return false;
}

// Split "/a/b/name" into its parent path and a pointer to "name" (inside
// `path`). False if there is no '/' or the parent does not fit.
static bool vfs_split_path(const char* path, char* parent_path, size_t parent_size, const char** name) {
    const char* last_slash = strrchr(path, '/');
    if (last_slash == NULL)
        return false;
    if (last_slash == path) {
        if (parent_size < 2) return false;
        strcpy(parent_path, "/");
    } else {
        size_t parent_len = (size_t)(last_slash - path);
        if (parent_len >= parent_size) return false;
        memcpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
    }
    *name = last_slash + 1;
    return true;
}

//
// Implementations (caller holds the VFS lock)
//
// Each public vfs_* function below takes the lock once and calls one of
// these, so a whole operation (resolve -> check -> modify) is atomic and an
// OpenFileDescriptor cannot be freed by vfs_close() while it is in use.
//

static File do_open(const char* path, uint16_t flags) {
    if (path == NULL || path[0] == '\0') 
        return VFS_INVALID_FILE;
    uint32_t current_knode = 0;
    uint32_t current_fs_node = 0;
    bool in_file_system = false;
    struct FSDeviceContext* ctx = NULL;

    if (!vfs_parse_path(path, flags, &current_knode, &current_fs_node, &in_file_system, &ctx)) {
        return VFS_INVALID_FILE;
    }
    
    if (in_file_system) {
        if (!fs_file_check(ctx, current_fs_node)) {
            return VFS_INVALID_FILE;
        }
        if (!vfs_fs_node_allowed(ctx, current_fs_node)) {
            return VFS_INVALID_FILE;    // Needs clearance the caller lacks
        }
    } else {
        uint8_t k_flags = kmalloc_get_flags(current_knode);
        if (k_flags & KMALLOC_FLAG_DIRECTORY) {
            return VFS_INVALID_FILE;
        }
    }
    
    OpenFileDescriptor* desc = (OpenFileDescriptor*)malloc(sizeof(OpenFileDescriptor));
    if (!desc) 
        return VFS_INVALID_FILE;

    desc->id = next_unique_id++;
    if (desc->id == VFS_INVALID_FILE) desc->id = next_unique_id++;  // skip 0 on wrap
    
    desc->in_file_system = in_file_system;
    desc->address = in_file_system ? current_fs_node : current_knode;
    desc->offset = 0;
    desc->flags = flags;
    desc->ctx = ctx;

    if (desc->in_file_system) {
        uint8_t perm = 0;
        fs_file_get_permissions(desc->ctx, desc->address, &perm);
        
        uint8_t target_mode = 0;
        if ((flags & VFS_OPEN_READ) && (perm & FS_PERMISSION_READ))    target_mode |= FS_FILE_MODE_READ;
        if ((flags & VFS_OPEN_WRITE) && (perm & FS_PERMISSION_WRITE))  target_mode |= FS_FILE_MODE_WRITE;
        if (target_mode == 0) {
            target_mode = FS_FILE_MODE_READ;
        }
        
        if (!fs_file_open(&desc->handle, desc->ctx, desc->address, target_mode)) {
            free(desc);
            return VFS_INVALID_FILE;
        }
    }
    
    if (!list_append(&open_files_head, &open_files_tail, desc)) {
        if (desc->in_file_system) {
            fs_file_close(&desc->handle);
        }
        free(desc);
        return VFS_INVALID_FILE;
    }
    
    return desc->id;
}

static void do_close(File file) {
    OpenFileDescriptor* desc = vfs_file_find_open(file);
    if (!desc) return;
    if (desc->in_file_system) {
        fs_file_close(&desc->handle);
    }
    
    list_remove(&open_files_head, &open_files_tail, desc);
    free(desc);
}

static int32_t do_read(File file, void* buffer, uint32_t size) {
    OpenFileDescriptor* desc = vfs_file_find_open(file);
    if (!desc) return -1;
    if (desc->in_file_system) {
        uint8_t parent_perm = 0;
        fs_file_get_permissions(desc->ctx, desc->address, &parent_perm);
        if (!(parent_perm & FS_PERMISSION_READ)) 
            return -1;
        if (!vfs_fs_node_allowed(desc->ctx, desc->address))
            return -1;
        return fs_file_read(&desc->handle, buffer, size);
    } else {
        uint8_t parent_perm = 0;
        parent_perm = kmalloc_get_permissions(desc->address);
        
        if (!(parent_perm & KMALLOC_PERMISSION_READ)) 
            return -1;
        uint32_t node_size = kmalloc_get_size(desc->address);
        if (desc->offset >= node_size) return 0;
        if (desc->offset + size > node_size) {
            size = node_size - desc->offset;
        }
        
        kmem_read(buffer, desc->address + desc->offset, size);
        desc->offset += size;
        return (int32_t)size;
    }
}

static int32_t do_write(File file, const void* buffer, uint32_t size) {
    OpenFileDescriptor* desc = vfs_file_find_open(file);
    if (!desc) return -1;
    
    if (desc->in_file_system) {
        uint8_t parent_perm = 0;
        fs_file_get_permissions(desc->ctx, desc->address, &parent_perm);
        if (!(parent_perm & FS_PERMISSION_WRITE)) 
            return -1;
        if (!vfs_fs_node_allowed(desc->ctx, desc->address))
            return -1;
        return fs_file_write(&desc->handle, buffer, size);
    } else {
        uint8_t parent_perm = 0;
        parent_perm = kmalloc_get_permissions(desc->address);
        
        if (!(parent_perm & KMALLOC_PERMISSION_WRITE)) 
            return -1;
        uint32_t node_size = kmalloc_get_size(desc->address);
        if (desc->offset >= node_size) return -1;
        if (desc->offset + size > node_size) {
            size = node_size - desc->offset;
        }
        
        kmem_write(desc->address + desc->offset, buffer, size);
        desc->offset += size;
        return (int32_t)size;
    }
}

static uint32_t do_seek(File file, uint32_t position) {
    OpenFileDescriptor* desc = vfs_file_find_open(file);
    if (!desc) return VFS_INVALID_FILE;
    
    if (desc->in_file_system) {
        fs_file_seek(&desc->handle, position);
    }
    
    desc->offset = position;
    return position;
}

static uint32_t do_tell(File file) {
    OpenFileDescriptor* desc = vfs_file_find_open(file);
    if (!desc) return VFS_INVALID_FILE;
    
    return (uint32_t)desc->offset;
}

static bool do_exists(const char* path) {
    if (path == NULL || path[0] == '\0') 
        return false;
    uint32_t address = resolve_path_to_address(path);
    if (address == 0xFFFFFFFF || address == 0) 
        return false;
    struct FSDeviceContext* ctx = vfs_device_get_context(path);
    if (ctx) {
        if (!fs_file_check(ctx, address)) 
            if (!fs_check_directory_valid(ctx, address)) 
                return false;
        return true;
    }
    return kmalloc_is_valid(address);
}

static bool do_mkdir(const char* path) {
    if (!vfs_path_fits(path)) 
        return false;
    char parent_path[VFS_PATH_MAX];
    const char* target_name = NULL;
    if (!vfs_split_path(path, parent_path, sizeof(parent_path), &target_name))
        return false;
    
    // Reject (rather than silently truncate) names that cannot be stored
    if (!vfs_name_is_valid(target_name))
        return false;
    
    uint32_t parent = resolve_path_to_address(parent_path);
    if (parent == 0xFFFFFFFF || parent == 0) {
        return false;
    }
    
    struct FSDeviceContext* ctx = vfs_device_get_context(parent_path);
    if (ctx && fs_check_directory_valid(ctx, parent)) {
        uint8_t parent_perm = 0;
        fs_file_get_permissions(ctx, parent, &parent_perm);
        
        if (!(parent_perm & FS_PERMISSION_READ) || 
            !(parent_perm & FS_PERMISSION_WRITE)) 
            return false;
        if (!vfs_fs_node_allowed(ctx, parent))
            return false;

        // Names are unique within a directory (files and directories alike).
        // The VFS lock makes this check-then-create atomic.
        if (fs_directory_find(ctx, parent, target_name) != FS_NULL)
            return false;

        return fs_directory_create(ctx, target_name, FS_PERMISSION_READ | FS_PERMISSION_WRITE, parent) != FS_NULL;
    }
    else if (knode_check_is_valid(parent) && !(kmalloc_get_flags(parent) & KMALLOC_FLAG_MOUNT)) {
        // Kernel nodes keep their short names. (A mount knode reached here
        // has an unusable device: its references are not entries.)
        char knode_name[VFS_KNODE_NAME_MAX];
        size_t length = strlen(target_name);
        if (length >= sizeof(knode_name))
            return false;
        memcpy(knode_name, target_name, length + 1);

        uint8_t parent_perm = 0;
        knode_get_permissions(parent, &parent_perm);

        if (!(parent_perm & KMALLOC_PERMISSION_READ) ||
            !(parent_perm & KMALLOC_PERMISSION_WRITE))
            return false;

        uint32_t existing = knode_find_by_name(parent, knode_name);
        if (existing != KNODE_NULL && existing != 0)
            return false;

        // TODO: check create_knode's result once it reports failure
        create_knode(knode_name, parent);
        return true;
    }
    return false;
}

static bool do_remove(const char* path) {
    if (path == NULL || path[0] == '\0') 
        return false;
    uint32_t address = resolve_path_to_address(path);
    if (address == 0xFFFFFFFF || address == 0) 
        return false;
    uint32_t parent = resolve_parent_path_to_address(path);
    if (parent == 0xFFFFFFFF || parent == 0) 
        return false;
    
    struct FSDeviceContext* ctx = vfs_device_get_context(path);
    if (ctx && fs_check_directory_valid(ctx, parent)) {
        uint8_t item_perm = 0;
        uint8_t parent_perm = 0;
        fs_file_get_permissions(ctx, address, &item_perm);
        fs_file_get_permissions(ctx, parent, &parent_perm);
        
        if (!(item_perm & FS_PERMISSION_WRITE) || 
            !(parent_perm & FS_PERMISSION_READ) || 
            !(parent_perm & FS_PERMISSION_WRITE)) 
            return false;
        // Clearance for the directory, the object and all it contains
        if (!vfs_fs_node_allowed(ctx, parent) || !vfs_remove_allowed(ctx, address))
            return false;
        // Never free something an open handle still points into
        if (vfs_fs_node_in_use(ctx, address))
            return false;
        
        if (fs_file_check(ctx, address)) {
            // Unlink first: a crash then leaks blocks instead of leaving an
            // entry that points at freed storage
            if (fs_directory_remove_entry(ctx, parent, address) != FS_DIR_OK)
                return false;
            return fs_file_delete(ctx, address);
        }
        
        // Directories go with everything inside them; basefs unlinks the
        // directory from its parent before freeing anything
        if (fs_check_directory_valid(ctx, address))
            return fs_directory_delete_recursive(ctx, address, parent);
        
        return false;
    } else {
        uint8_t item_perm = 0;
        uint8_t parent_perm = 0;
        knode_get_permissions(address, &item_perm);
        knode_get_permissions(parent, &parent_perm);
        
        if (!(item_perm & KMALLOC_PERMISSION_WRITE) || 
            !(parent_perm & KMALLOC_PERMISSION_READ) || 
            !(parent_perm & KMALLOC_PERMISSION_WRITE)) 
            return false;
        if (destroy_knode(address, parent)) 
            return true;
    }
    
    return false;
}

static bool do_rename(const char* path, const char* name) {
    if (path == NULL || path[0] == '\0' || path[0] == ' ') 
        return false;
    if (!vfs_name_is_valid(name)) 
        return false;
    
    uint32_t address = resolve_path_to_address(path);
    if (address == 0xFFFFFFFF || address == 0) 
        return false;
    
    uint32_t parent_address = resolve_parent_path_to_address(path);
    if (parent_address == 0xFFFFFFFF || parent_address == 0) {
        return false;
    }
    
    struct FSDeviceContext* ctx = vfs_device_get_context(path);
    if (ctx && fs_check_directory_valid(ctx, parent_address)) {
        uint8_t item_perm = 0;
        fs_file_get_permissions(ctx, address, &item_perm);
        if (!(item_perm & FS_PERMISSION_WRITE))
            return false;
        if (!vfs_fs_node_allowed(ctx, address) || !vfs_fs_node_allowed(ctx, parent_address))
            return false;
        // The name lives in the parent's entry. The rename refuses a name
        // another entry already has (the VFS lock makes that atomic).
        return fs_directory_rename_entry(ctx, parent_address, address, name) == FS_DIR_OK;
    } else {
        // Kernel nodes. Never a mount: its name is the device's name, and the
        // storage scan re-attaches mounts by it.
        if (!kmalloc_is_valid(address) || !kmalloc_is_valid(parent_address))
            return false;
        if (kmalloc_get_flags(address) & KMALLOC_FLAG_MOUNT)
            return false;
        if (!(kmalloc_get_permissions(address) & KMALLOC_PERMISSION_WRITE) ||
            !(kmalloc_get_permissions(parent_address) & KMALLOC_PERMISSION_WRITE))
            return false;
        
        // Kernel nodes keep their short names, unique within their folder
        char knode_name[VFS_KNODE_NAME_MAX];
        size_t length = strlen(name);
        if (length >= sizeof(knode_name))
            return false;
        memcpy(knode_name, name, length + 1);
        
        uint32_t existing = knode_find_by_name(parent_address, knode_name);
        if (existing != KNODE_NULL && existing != 0)
            return existing == address;
        
        knode_set_name(address, knode_name);
        return true;
    }
}

static bool do_truncate(const char* path, uint32_t new_size) {
    if (path == NULL) 
        return false;
    if (path[0] == '\0' || path[0] == ' ') 
        return false;
    uint32_t address = resolve_path_to_address(path);
    if (address == 0xFFFFFFFF || address == 0) 
        return false;
    
    struct FSDeviceContext* ctx = vfs_device_get_context(path);
    // fs_file_resize refuses directories and reports allocation failure
    // (the file is left unchanged in that case), so pass its result on
    if (ctx) {
        if (!vfs_fs_node_allowed(ctx, address))
            return false;
        return fs_file_resize(ctx, address, new_size);
    }
    return false;
}

static bool do_stat(const char* path, FSFileStats* stats) {
    if (path == NULL || path[0] == '\0' || stats == NULL) 
        return false;
    uint32_t address = resolve_path_to_address(path);
    if (address == 0xFFFFFFFF || address == 0) 
        return false;
    memset(stats, 0, sizeof(FSFileStats));
    
    struct FSDeviceContext* ctx = vfs_device_get_context(path);
    if (ctx) {
        bool is_directory = fs_check_directory_valid(ctx, address);
        if (!is_directory && !fs_file_check(ctx, address))
            return false;
        
        uint8_t fs_perm = 0;
        fs_file_get_permissions(ctx, address, &fs_perm);
        if (fs_perm & FS_PERMISSION_EXECUTE) stats->permissions |= VFS_PERMISSION_EXECUTE;
        if (fs_perm & FS_PERMISSION_READ)    stats->permissions |= VFS_PERMISSION_READ;
        if (fs_perm & FS_PERMISSION_WRITE)   stats->permissions |= VFS_PERMISSION_WRITE;
        
        if (is_directory) {
            // The device's root is the mount as seen from outside
            stats->type = (fs_directory_get_parent(ctx, address) == FS_NULL) ? VFS_TYPE_MOUNT
                                                                              : VFS_TYPE_DIRECTORY;
        } else {
            stats->type = VFS_TYPE_FILE;
            stats->size = fs_file_get_size(ctx, address);
        }
        
        fs_file_get_certificate(ctx, address, &stats->certificate);
        fs_file_get_times(ctx, address, &stats->created, &stats->modified);
        return true;
    }
    
    // Kernel nodes: folders, device values (/dev/...), and mounts whose
    // device is not usable
    if (kmalloc_is_valid(address)) {
        uint8_t k_perm = kmalloc_get_permissions(address);
        if (k_perm & KMALLOC_PERMISSION_EXECUTABLE) stats->permissions |= VFS_PERMISSION_EXECUTE;
        if (k_perm & KMALLOC_PERMISSION_READ)       stats->permissions |= VFS_PERMISSION_READ;
        if (k_perm & KMALLOC_PERMISSION_WRITE)      stats->permissions |= VFS_PERMISSION_WRITE;
        
        uint8_t k_flags = kmalloc_get_flags(address);
        if (k_flags & KMALLOC_FLAG_MOUNT) {
            stats->type = VFS_TYPE_MOUNT;
        } else if (k_flags & KMALLOC_FLAG_DIRECTORY) {
            stats->type = VFS_TYPE_DIRECTORY;
        } else {
            stats->type = VFS_TYPE_FILE;
            stats->size = kmalloc_get_size(address);
        }
        return true;
    }
    
    return false;
}

static uint32_t do_get_size(File file) {
    OpenFileDescriptor* desc = vfs_file_find_open(file);
    if (!desc) return 0;
    if (desc->in_file_system) {
        return fs_file_get_size(desc->ctx, desc->address);
    } else {
        return kmalloc_get_size(desc->address);
    }
}

//
// Public API: take the lock, run the operation, release
//

File vfs_open(const char* path, uint16_t flags) {
    vfs_lock();
    File result = do_open(path, flags);
    vfs_unlock();
    return result;
}

void vfs_close(File file) {
    vfs_lock();
    do_close(file);
    vfs_unlock();
}

int32_t vfs_read(File file, void* buffer, uint32_t size) {
    vfs_lock();
    int32_t result = do_read(file, buffer, size);
    vfs_unlock();
    return result;
}

int32_t vfs_write(File file, const void* buffer, uint32_t size) {
    vfs_lock();
    int32_t result = do_write(file, buffer, size);
    vfs_unlock();
    return result;
}

uint32_t vfs_seek(File file, uint32_t position) {
    vfs_lock();
    uint32_t result = do_seek(file, position);
    vfs_unlock();
    return result;
}

uint32_t vfs_tell(File file) {
    vfs_lock();
    uint32_t result = do_tell(file);
    vfs_unlock();
    return result;
}

bool vfs_exists(const char* path) {
    vfs_lock();
    bool result = do_exists(path);
    vfs_unlock();
    return result;
}

bool vfs_mkdir(const char* path) {
    vfs_lock();
    bool result = do_mkdir(path);
    vfs_unlock();
    return result;
}

bool vfs_remove(const char* path) {
    vfs_lock();
    bool result = do_remove(path);
    vfs_unlock();
    return result;
}

bool vfs_rename(const char* path, const char* name) {
    vfs_lock();
    bool result = do_rename(path, name);
    vfs_unlock();
    return result;
}

bool vfs_truncate(const char* path, uint32_t new_size) {
    vfs_lock();
    bool result = do_truncate(path, new_size);
    vfs_unlock();
    return result;
}

bool vfs_stat(const char* path, FSFileStats* stats) {
    vfs_lock();
    bool result = do_stat(path, stats);
    vfs_unlock();
    return result;
}

uint32_t vfs_get_size(File file) {
    vfs_lock();
    uint32_t result = do_get_size(file);
    vfs_unlock();
    return result;
}



