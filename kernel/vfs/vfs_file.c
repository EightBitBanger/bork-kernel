#include <stdint.h>
#include <stdbool.h>
#include <kernel/malloc.h>

#include <kernel/knode.h>

#include <kernel/vfs/vfs_internal.h>

#include <kernel/util/string.h>
#include <kernel/util/tok.h>
#include <kernel/util/list.h>

// Name validation (vfs_name_is_valid) lives in vfs_parse.c, shared with
// path resolution.

// True if an open file sits at `address` or anywhere below it on `mount`.
// Freeing such an object would leave the handle pointing at freed blocks.
// Walks each open file's parent chain.
static bool vfs_fs_node_in_use(struct FsMount* mount, uint64_t address) {
    for (struct list_node* n = open_files_head; n != NULL; n = n->next) {
        OpenFileDescriptor* desc = (OpenFileDescriptor*)n->data;
        if (desc == NULL || !desc->in_file_system || desc->mount != mount)
            continue;
        uint64_t node  = desc->address;
        uint32_t guard = 1024;      // Far deeper than any real tree
        while (node != FS_NODE_NULL && node != 0 && guard-- > 0) {
            if (node == address)
                return true;
            node = mount->type->parent(mount, node);
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

    // Truncating and appending change the file: they only make sense on a
    // handle that may write
    bool modifying = (flags & (VFS_OPEN_TRUNCATE | VFS_OPEN_APPEND)) != 0;
    if (modifying && !(flags & VFS_OPEN_WRITE))
        return VFS_INVALID_FILE;

    void* current_knode = NULL;
    uint64_t current_fs_node = 0;
    bool in_file_system = false;
    struct FsMount* mount = NULL;

    if (!vfs_parse_path(path, flags, &current_knode, &current_fs_node, &in_file_system, &mount)) {
        return VFS_INVALID_FILE;
    }
    
    if (in_file_system) {
        if (!vfs_node_is_file(mount, current_fs_node)) {
            return VFS_INVALID_FILE;
        }
        if (!vfs_fs_node_allowed(mount, current_fs_node)) {
            return VFS_INVALID_FILE;    // Needs clearance the caller lacks
        }
        if (modifying) {
            // Refuse rather than open read-only and drop the request
            uint8_t perm = vfs_node_permissions(mount, current_fs_node);
            if (!(perm & VFS_PERMISSION_WRITE))
                return VFS_INVALID_FILE;
        }
    } else {
        uint8_t k_flags = kmalloc_get_flags(current_knode);
        if (k_flags & KMALLOC_FLAG_DIRECTORY) {
            return VFS_INVALID_FILE;
        }
        // Kernel nodes are fixed-size blocks: they can neither be emptied
        // nor grown at the end
        if (modifying) {
            return VFS_INVALID_FILE;
        }
    }
    
    OpenFileDescriptor* desc = (OpenFileDescriptor*)malloc(sizeof(OpenFileDescriptor));
    if (!desc) 
        return VFS_INVALID_FILE;

    desc->id = next_unique_id++;
    if (desc->id == VFS_INVALID_FILE) desc->id = next_unique_id++;  // skip 0 on wrap
    
    desc->in_file_system = in_file_system;
    desc->address = in_file_system ? current_fs_node : 0;
    desc->knode = in_file_system ? NULL : current_knode;
    desc->offset = 0;
    desc->flags = flags;
    desc->mount = mount;
    desc->handle = NULL;

    if (desc->in_file_system) {
        FsNodeAttr attr;
        if (!mount->type->get_attr(mount, desc->address, &attr)) {
            free(desc);
            return VFS_INVALID_FILE;
        }
        
        uint16_t target_mode = 0;
        if ((flags & VFS_OPEN_READ) && (attr.permissions & VFS_PERMISSION_READ))    target_mode |= VFS_OPEN_READ;
        if ((flags & VFS_OPEN_WRITE) && (attr.permissions & VFS_PERMISSION_WRITE))  target_mode |= VFS_OPEN_WRITE;
        if (target_mode == 0) {
            target_mode = VFS_OPEN_READ;
        }
        if (flags & VFS_OPEN_APPEND) {
            target_mode |= VFS_OPEN_APPEND;     // Handle starts at the end
        }
        
        // Every check has passed and the descriptor exists: only now empty
        // the file, so a refused open never loses data. (basefs: the resize
        // bumps the chain generation, so other handles on this file drop
        // their cached position safely.)
        if ((flags & VFS_OPEN_TRUNCATE) && attr.size != 0 &&
            !mount->type->resize(mount, desc->address, 0)) {
            free(desc);
            return VFS_INVALID_FILE;
        }
        
        desc->handle = mount->type->open(mount, desc->address, target_mode);
        if (desc->handle == NULL) {
            free(desc);
            return VFS_INVALID_FILE;
        }
        desc->offset = mount->type->tell(desc->handle);
    }
    
    if (!list_append(&open_files_head, &open_files_tail, desc)) {
        if (desc->in_file_system) {
            mount->type->close(desc->handle);
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
        desc->mount->type->close(desc->handle);
    }
    
    list_remove(&open_files_head, &open_files_tail, desc);
    free(desc);
}

static int32_t do_read(File file, void* buffer, uint32_t size) {
    OpenFileDescriptor* desc = vfs_file_find_open(file);
    if (!desc) return -1;
    if (desc->in_file_system) {
        struct FsMount* mount = desc->mount;
        uint8_t parent_perm = vfs_node_permissions(mount, desc->address);
        if (!(parent_perm & VFS_PERMISSION_READ)) 
            return -1;
        if (!vfs_fs_node_allowed(mount, desc->address))
            return -1;
        int32_t got = (int32_t)mount->type->read(desc->handle, buffer, size);
        desc->offset = mount->type->tell(desc->handle);
        return got;
    } else {
        uint8_t parent_perm = 0;
        parent_perm = kmalloc_get_permissions(desc->knode);
        
        if (!(parent_perm & KMALLOC_PERMISSION_READ)) 
            return -1;
        uint32_t node_size = kmalloc_get_size(desc->knode);
        if (desc->offset >= node_size) return 0;
        if (desc->offset + size > node_size) {
            size = node_size - desc->offset;
        }
        
        memcpy(buffer, (const uint8_t*)desc->knode + desc->offset, size);
        desc->offset += size;
        return (int32_t)size;
    }
}

static int32_t do_write(File file, const void* buffer, uint32_t size) {
    OpenFileDescriptor* desc = vfs_file_find_open(file);
    if (!desc) return -1;
    
    if (desc->in_file_system) {
        struct FsMount* mount = desc->mount;
        FsNodeAttr attr;
        if (!mount->type->get_attr(mount, desc->address, &attr) ||
            !(attr.permissions & VFS_PERMISSION_WRITE)) 
            return -1;
        if (!vfs_fs_node_allowed(mount, desc->address))
            return -1;
        // Append: the end as it is now (another handle may have grown the
        // file since the last write). The write grows it from there.
        if (desc->flags & VFS_OPEN_APPEND)
            mount->type->seek(desc->handle, attr.size);
        int32_t written = (int32_t)mount->type->write(desc->handle, buffer, size);
        desc->offset = mount->type->tell(desc->handle);
        return written;
    } else {
        uint8_t parent_perm = 0;
        parent_perm = kmalloc_get_permissions(desc->knode);
        
        if (!(parent_perm & KMALLOC_PERMISSION_WRITE)) 
            return -1;
        uint32_t node_size = kmalloc_get_size(desc->knode);
        if (desc->offset >= node_size) return -1;
        if (desc->offset + size > node_size) {
            size = node_size - desc->offset;
        }
        
        memcpy((uint8_t*)desc->knode + desc->offset, buffer, size);
        desc->offset += size;
        return (int32_t)size;
    }
}

static uint64_t do_seek(File file, uint64_t position) {
    OpenFileDescriptor* desc = vfs_file_find_open(file);
    if (!desc) return VFS_INVALID_FILE;
    
    if (desc->in_file_system) {
        desc->mount->type->seek(desc->handle, position);
    }
    
    desc->offset = position;
    return position;
}

static uint64_t do_tell(File file) {
    OpenFileDescriptor* desc = vfs_file_find_open(file);
    if (!desc) return VFS_INVALID_FILE;
    
    return desc->offset;
}

static bool do_exists(const char* path) {
    if (path == NULL || path[0] == '\0') 
        return false;
    uint64_t address = resolve_path_to_address(path);
    if (vfs_address_is_null(address)) 
        return false;
    struct FsMount* mount = vfs_device_get_context(path);
    if (mount)
        return vfs_node_type(mount, address) != FS_NODE_NONE;
    return kmalloc_is_valid(vfs_address_knode(address));
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
    
    uint64_t parent = resolve_path_to_address(parent_path);
    if (vfs_address_is_null(parent)) {
        return false;
    }
    
    struct FsMount* mount = vfs_device_get_context(parent_path);
    if (mount && vfs_node_is_directory(mount, parent)) {
        uint8_t parent_perm = vfs_node_permissions(mount, parent);
        
        if (!(parent_perm & VFS_PERMISSION_READ) || 
            !(parent_perm & VFS_PERMISSION_WRITE)) 
            return false;
        if (!vfs_fs_node_allowed(mount, parent))
            return false;

        // Names are unique within a directory (files and directories alike).
        // The VFS lock makes this check-then-create atomic.
        if (mount->type->lookup(mount, parent, target_name) != FS_NODE_NULL)
            return false;

        return mount->type->create(mount, parent, target_name, FS_NODE_DIRECTORY,
                                   VFS_PERMISSION_READ | VFS_PERMISSION_WRITE) != FS_NODE_NULL;
    }
    
    void* parent_knode = mount ? NULL : vfs_address_knode(parent);
    if (knode_check_is_valid(parent_knode) && !(kmalloc_get_flags(parent_knode) & KMALLOC_FLAG_MOUNT)) {
        // Kernel nodes keep their short names. (A mount knode reached here
        // has an unusable device: its references are not entries.)
        char knode_name[VFS_KNODE_NAME_MAX];
        size_t length = strlen(target_name);
        if (length >= sizeof(knode_name))
            return false;
        memcpy(knode_name, target_name, length + 1);

        uint8_t parent_perm = 0;
        knode_get_permissions(parent_knode, &parent_perm);

        if (!(parent_perm & KMALLOC_PERMISSION_READ) ||
            !(parent_perm & KMALLOC_PERMISSION_WRITE))
            return false;

        if (knode_find_by_name(parent_knode, knode_name) != NULL)
            return false;

        return create_knode(knode_name, parent_knode) != NULL;
    }
    return false;
}

static bool do_remove(const char* path) {
    if (path == NULL || path[0] == '\0') 
        return false;
    uint64_t address = resolve_path_to_address(path);
    if (vfs_address_is_null(address)) 
        return false;
    uint64_t parent = resolve_parent_path_to_address(path);
    if (vfs_address_is_null(parent)) 
        return false;
    
    struct FsMount* mount = vfs_device_get_context(path);
    if (mount && vfs_node_is_directory(mount, parent)) {
        uint8_t item_perm = vfs_node_permissions(mount, address);
        uint8_t parent_perm = vfs_node_permissions(mount, parent);
        
        if (!(item_perm & VFS_PERMISSION_WRITE) || 
            !(parent_perm & VFS_PERMISSION_READ) || 
            !(parent_perm & VFS_PERMISSION_WRITE)) 
            return false;
        // Clearance for the directory, the object and all it contains
        if (!vfs_fs_node_allowed(mount, parent) || !vfs_remove_allowed(mount, address))
            return false;
        // Never free something an open handle still points into
        if (vfs_fs_node_in_use(mount, address))
            return false;
        
        // A directory goes with everything inside it
        if (vfs_node_type(mount, address) == FS_NODE_NONE)
            return false;
        return mount->type->remove(mount, parent, address);
    } else {
        // A device's root (its parent is the /mnt knode) is not a knode to destroy
        if (mount)
            return false;
        void* knode = vfs_address_knode(address);
        void* parent_knode = vfs_address_knode(parent);
        uint8_t item_perm = 0;
        uint8_t parent_perm = 0;
        knode_get_permissions(knode, &item_perm);
        knode_get_permissions(parent_knode, &parent_perm);
        
        if (!(item_perm & KMALLOC_PERMISSION_WRITE) || 
            !(parent_perm & KMALLOC_PERMISSION_READ) || 
            !(parent_perm & KMALLOC_PERMISSION_WRITE)) 
            return false;
        if (destroy_knode(knode, parent_knode)) 
            return true;
    }
    
    return false;
}

static bool do_rename(const char* path, const char* name) {
    if (path == NULL || path[0] == '\0' || path[0] == ' ') 
        return false;
    if (!vfs_name_is_valid(name)) 
        return false;
    
    uint64_t address = resolve_path_to_address(path);
    if (vfs_address_is_null(address)) 
        return false;
    
    uint64_t parent_address = resolve_parent_path_to_address(path);
    if (vfs_address_is_null(parent_address)) {
        return false;
    }
    
    struct FsMount* mount = vfs_device_get_context(path);
    if (mount && vfs_node_is_directory(mount, parent_address)) {
        uint8_t item_perm = vfs_node_permissions(mount, address);
        if (!(item_perm & VFS_PERMISSION_WRITE))
            return false;
        if (!vfs_fs_node_allowed(mount, address) || !vfs_fs_node_allowed(mount, parent_address))
            return false;
        // The name lives in the parent's entry. The rename refuses a name
        // another entry already has (the VFS lock makes that atomic).
        return mount->type->rename(mount, parent_address, address, name);
    } else {
        // Kernel nodes. Never a mount: its name is the device's name, and the
        // storage scan re-attaches mounts by it.
        if (mount)
            return false;   // A device's root: it is named by its mount
        void* knode = vfs_address_knode(address);
        void* parent_knode = vfs_address_knode(parent_address);
        if (!kmalloc_is_valid(knode) || !kmalloc_is_valid(parent_knode))
            return false;
        if (kmalloc_get_flags(knode) & KMALLOC_FLAG_MOUNT)
            return false;
        if (!(kmalloc_get_permissions(knode) & KMALLOC_PERMISSION_WRITE) ||
            !(kmalloc_get_permissions(parent_knode) & KMALLOC_PERMISSION_WRITE))
            return false;
        
        // Kernel nodes keep their short names, unique within their folder
        char knode_name[VFS_KNODE_NAME_MAX];
        size_t length = strlen(name);
        if (length >= sizeof(knode_name))
            return false;
        memcpy(knode_name, name, length + 1);
        
        void* existing = knode_find_by_name(parent_knode, knode_name);
        if (existing != NULL)
            return existing == knode;
        
        knode_set_name(knode, knode_name);
        return true;
    }
}

static bool do_truncate(const char* path, uint64_t new_size) {
    if (path == NULL) 
        return false;
    if (path[0] == '\0' || path[0] == ' ') 
        return false;
    uint64_t address = resolve_path_to_address(path);
    if (vfs_address_is_null(address)) 
        return false;
    
    struct FsMount* mount = vfs_device_get_context(path);
    // The resize refuses directories and reports allocation failure (the
    // file is left unchanged in that case), so pass its result on
    if (mount) {
        if (!vfs_fs_node_allowed(mount, address))
            return false;
        return mount->type->resize(mount, address, new_size);
    }
    return false;
}

static bool do_stat(const char* path, FSFileStats* stats) {
    if (path == NULL || path[0] == '\0' || stats == NULL) 
        return false;
    uint64_t address = resolve_path_to_address(path);
    if (vfs_address_is_null(address)) 
        return false;
    memset(stats, 0, sizeof(FSFileStats));
    
    struct FsMount* mount = vfs_device_get_context(path);
    if (mount) {
        uint8_t node_type = vfs_node_type(mount, address);
        FsNodeAttr attr;
        if (node_type == FS_NODE_NONE || !mount->type->get_attr(mount, address, &attr))
            return false;
        
        stats->permissions = attr.permissions;
        
        if (node_type == FS_NODE_DIRECTORY) {
            // The device's root is the mount as seen from outside
            stats->type = (mount->type->parent(mount, address) == FS_NODE_NULL) ? VFS_TYPE_MOUNT
                                                                                 : VFS_TYPE_DIRECTORY;
        } else {
            stats->type = VFS_TYPE_FILE;
            stats->size = attr.size;
        }
        
        stats->certificate = attr.certificate;
        stats->created     = attr.created;
        stats->modified    = attr.modified;
        return true;
    }
    
    // Kernel nodes: folders, device values (/dev/...), and mounts whose
    // device is not usable
    void* knode = vfs_address_knode(address);
    if (kmalloc_is_valid(knode)) {
        uint8_t k_perm = kmalloc_get_permissions(knode);
        if (k_perm & KMALLOC_PERMISSION_EXECUTABLE) stats->permissions |= VFS_PERMISSION_EXECUTE;
        if (k_perm & KMALLOC_PERMISSION_READ)       stats->permissions |= VFS_PERMISSION_READ;
        if (k_perm & KMALLOC_PERMISSION_WRITE)      stats->permissions |= VFS_PERMISSION_WRITE;
        
        uint8_t k_flags = kmalloc_get_flags(knode);
        if (k_flags & KMALLOC_FLAG_MOUNT) {
            stats->type = VFS_TYPE_MOUNT;
        } else if (k_flags & KMALLOC_FLAG_DIRECTORY) {
            stats->type = VFS_TYPE_DIRECTORY;
        } else {
            stats->type = VFS_TYPE_FILE;
            stats->size = kmalloc_get_size(knode);
        }
        return true;
    }
    
    return false;
}

static uint64_t do_get_size(File file) {
    OpenFileDescriptor* desc = vfs_file_find_open(file);
    if (!desc) return 0;
    if (desc->in_file_system) {
        FsNodeAttr attr;
        return desc->mount->type->get_attr(desc->mount, desc->address, &attr) ? attr.size : 0;
    } else {
        return kmalloc_get_size(desc->knode);
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

uint64_t vfs_seek(File file, uint64_t position) {
    vfs_lock();
    uint64_t result = do_seek(file, position);
    vfs_unlock();
    return result;
}

uint64_t vfs_tell(File file) {
    vfs_lock();
    uint64_t result = do_tell(file);
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

bool vfs_truncate(const char* path, uint64_t new_size) {
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

uint64_t vfs_get_size(File file) {
    vfs_lock();
    uint64_t result = do_get_size(file);
    vfs_unlock();
    return result;
}
