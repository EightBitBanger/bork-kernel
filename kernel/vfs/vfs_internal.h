#ifndef _VIRTUAL_FILE_SYSTEM_INTERNAL_H_
#define _VIRTUAL_FILE_SYSTEM_INTERNAL_H_

#include <kernel/vfs/vfs.h>
#include <kernel/fs/fs_type.h>

//
// Locking
//
// One recursive sleeping lock (vfs_lock / vfs_unlock, see vfs_lock.c)
// serializes the whole VFS:
//
//   - the open-file table (open_files_head/tail, next_unique_id) and every
//     OpenFileDescriptor in it
//   - path resolution and knode changes made through the VFS
//   - every mounted volume: its FsMount, the filesystem's own state behind
//     it (caches, windows) and the on-disk structures
//
// Filesystems are deliberately NOT thread-safe: they know nothing about
// threads and stay portable. Their contract (fs/fs_type.h) is: every FsType
// call on a mounted volume is made with the VFS lock held. Public vfs_* functions take the lock; every
// function declared in this header expects the caller to already hold it.
//
// It is a mutex rather than an irq_save() section because disk I/O happens
// underneath it. With IF=0 the timer stops, timer_get_ms() freezes and no
// other thread runs for the length of each sector transfer.
//
// Lock order: kernel_big_lock (if held) -> VFS lock -> driver locks.
// Never call into anything that takes kernel_big_lock while
// holding the VFS lock, and never take it from interrupt context.
//

// Addresses
//
// A resolved address is either a knode (a kernel pointer) or, inside a
// mounted device, a node of that volume's filesystem (fs/fs_type.h); which
// one is decided by vfs_device_get_context (see below), never by the value.
// Both kinds travel as uint64_t: convert a knode with vfs_knode_address
// and back with vfs_address_knode, and only after vfs_device_get_context
// said there is no device. "Nothing there" is 0 (a NULL knode) or FS_NODE_NULL.
static inline bool vfs_address_is_null(uint64_t address) {
    return address == 0 || address == FS_NODE_NULL;
}

static inline uint64_t vfs_knode_address(const void* knode) {
    return (uint64_t)(uintptr_t)knode;
}

// NULL for anything that cannot be a kernel pointer
static inline void* vfs_address_knode(uint64_t address) {
    if (vfs_address_is_null(address) || address > (uint64_t)UINTPTR_MAX)
        return NULL;
    return (void*)(uintptr_t)address;
}

typedef struct {
    File id;
    uint64_t address;               // Filesystem node (in_file_system)
    void* knode;                    // Kernel node (!in_file_system)
    bool in_file_system;
    uint64_t offset;
    void* handle;                   // The filesystem's open handle (in_file_system)
    uint16_t flags;
    struct FsMount* mount;          // Volume the node is on (in_file_system)
} OpenFileDescriptor;

extern struct list_node* open_files_head;
extern struct list_node* open_files_tail;
extern File next_unique_id;

// Kernel node names: knodes store at most this many bytes including the
// terminator (the limit the VFS has always enforced on create/rename).
#define VFS_KNODE_NAME_MAX  16

// Name rules shared by every VFS entry point: 1..VFS_NAME_MAX bytes, no
// '/', no control characters, no leading space, not "." or ".."
bool vfs_name_is_valid(const char* name);

// True if `path` is non-empty and shorter than VFS_PATH_MAX
bool vfs_path_fits(const char* path);

// Caller must hold the VFS lock for everything below

// Return FS_NODE_NULL when the path does not exist (test with vfs_address_is_null)
uint64_t resolve_path_to_address(const char* path);
uint64_t resolve_parent_path_to_address(const char* path);
// Always a knode (NULL when the path does not exist)
void* resolve_path_to_mount_point(const char* path);
bool vfs_parse_path(const char* path, uint16_t flags, void** out_knode, uint64_t* out_fs_node, bool* out_in_fs, struct FsMount** out_mount);
// The device `path` lies on, only while that device is open (formatted and
// attached). An address resolved from `path` belongs to this context exactly
// when it is non-NULL; otherwise the address is a knode. Every path-based
// operation decides "device or knode" this way.
struct FsMount* vfs_device_get_context(const char* path);

// The context of the mount `path` lies under, whatever state it is in
// (unformatted, detached). Only for operations on the mount itself:
// format, partition, device info.
struct FsMount* vfs_device_get_mount_context(const char* path);

// The returned descriptor is only valid while the caller keeps holding the
// VFS lock; once it is released another thread may vfs_close() and free it.
OpenFileDescriptor* vfs_file_find_open(File id);

bool vfs_directory_check_mounted(const char* path);

// Security clearance (vfs_security.c). True when the caller may use it.
//   vfs_fs_node_allowed   one object on `mount` (no object there: true)
//   vfs_remove_allowed    the object and, for a directory, everything below
//   vfs_path_allowed      the object at `path` and every directory on the
//                         way to it (kernel nodes: true)
//   vfs_parent_allowed    the directory holding `path`, and its ancestors
bool vfs_fs_node_allowed(struct FsMount* mount, uint64_t address);
bool vfs_remove_allowed(struct FsMount* mount, uint64_t address);
bool vfs_path_allowed(const char* path);
bool vfs_parent_allowed(const char* path);

// Mounts (vfs_mount.c)
//
// A mount is the knode /mnt/<device> (KMALLOC_FLAG_DIRECTORY |
// KMALLOC_FLAG_MOUNT) whose only reference is its struct FsMount.

// The /mnt folder, NULL before it exists
void* vfs_mnt_directory(void);

// The FsMount of a mount knode, NULL for any other knode
struct FsMount* vfs_knode_mount(void* knode);

// Nodes on an open volume

static inline uint8_t vfs_node_type(struct FsMount* mount, uint64_t node) {
    return mount->type->node_type(mount, node);
}

static inline bool vfs_node_is_file(struct FsMount* mount, uint64_t node) {
    return vfs_node_type(mount, node) == FS_NODE_FILE;
}

static inline bool vfs_node_is_directory(struct FsMount* mount, uint64_t node) {
    return vfs_node_type(mount, node) == FS_NODE_DIRECTORY;
}

// VFS_PERMISSION_* of a live node, 0 when there is none
static inline uint8_t vfs_node_permissions(struct FsMount* mount, uint64_t node) {
    FsNodeAttr attr;
    return mount->type->get_attr(mount, node, &attr) ? attr.permissions : 0;
}

#endif
