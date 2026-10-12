#ifndef _VIRTUAL_FILE_SYSTEM_INTERNAL_H_
#define _VIRTUAL_FILE_SYSTEM_INTERNAL_H_

#include <kernel/vfs/vfs.h>
#include <kernel/fs/fs.h>

//
// Locking
//
// One recursive sleeping lock (vfs_lock / vfs_unlock, see vfs_lock.c)
// serializes the whole VFS:
//
//   - the open-file table (open_files_head/tail, next_unique_id) and every
//     OpenFileDescriptor in it
//   - path resolution and knode changes made through the VFS
//   - every mounted basefs device: its FSDeviceContext sector cache and
//     bitmap window, and the on-disk structures behind them
//
// basefs is deliberately NOT thread-safe: it knows nothing about threads and
// stays portable. Its contract is: all fs_* calls on a mounted device must be
// made with the VFS lock held. Public vfs_* functions take the lock; every
// function declared in this header expects the caller to already hold it.
//
// It is a mutex rather than an irq_save() section because disk I/O happens
// underneath it. With IF=0 the timer stops, timer_get_ms() freezes and no
// other thread runs for the length of each sector transfer.
//
// Lock order: kernel_big_lock (if held) -> VFS lock -> driver locks.
// Never call into the DWM or anything that takes kernel_big_lock while
// holding the VFS lock, and never take it from interrupt context.
//

typedef struct {
    File id;
    uint32_t address;
    bool in_file_system;
    uint32_t offset;
    FileHandle handle;
    uint16_t flags;
    struct FSDeviceContext* ctx;
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

uint32_t resolve_path_to_address(const char* path);
uint32_t resolve_parent_path_to_address(const char* path);
uint32_t resolve_path_to_mount_point(const char* path);
bool vfs_parse_path(const char* path, uint16_t flags, uint32_t* out_knode, uint32_t* out_fs_node, bool* out_in_fs, struct FSDeviceContext** out_ctx);
// The device `path` lies on, only while that device is open (formatted and
// attached). An address resolved from `path` belongs to this context exactly
// when it is non-NULL; otherwise the address is a knode. Every path-based
// operation decides "device or knode" this way.
struct FSDeviceContext* vfs_device_get_context(const char* path);

// The context of the mount `path` lies under, whatever state it is in
// (unformatted, detached). Only for operations on the mount itself:
// format, partition, device info.
struct FSDeviceContext* vfs_device_get_mount_context(const char* path);
uint8_t* vfs_device_get_block(const char* path);

// The returned descriptor is only valid while the caller keeps holding the
// VFS lock; once it is released another thread may vfs_close() and free it.
OpenFileDescriptor* vfs_file_find_open(File id);

bool vfs_directory_check_mounted(const char* path);

#endif


