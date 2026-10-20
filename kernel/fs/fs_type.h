#ifndef _KERNEL_FS_TYPE_H_
#define _KERNEL_FS_TYPE_H_

// Filesystem types
//
// The VFS drives every mounted volume through one of these tables and never
// calls a filesystem by name, so it does not know which on-disk format a
// device holds. Adding a filesystem means writing one FsType and listing it
// in vfs_mount.c; nothing else in the VFS changes.
//
// Nodes
//
// A node (file or directory) on a volume is a 64-bit value the type hands
// out and takes back. The VFS only stores it, compares it and passes it on;
// what it means (basefs: a byte offset on the disk) is the type's business.
// FS_NODE_NULL is "no node". 0 is never a valid node either.
//
// Locking
//
// Every call is made with the VFS lock held, one at a time. Types need no
// locking of their own.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <kernel/vfs/vfs.h>

#define FS_NODE_NULL            0xFFFFFFFFFFFFFFFFULL

// FsType.node_type
#define FS_NODE_NONE            0   // No live object at that node
#define FS_NODE_FILE            1
#define FS_NODE_DIRECTORY       2

struct BlockDevice;
struct FsType;

// One mounted volume: the /mnt/<device> knode's only reference.
struct FsMount {
    const struct FsType* type;      // Never NULL
    struct BlockDevice*  device;    // NULL once detached (vfs_device_release_disk)
    bool                 is_open;   // The device holds a readable volume of `type`
    void*                data;      // The type's own state (attach allocates it)
};

typedef struct {
    uint64_t size;                  // Files only
    uint8_t  permissions;           // VFS_PERMISSION_*
    uint32_t security;              // VFS_SECURITY_* mask, 0 if the type has none
    uint32_t certificate;           // 0 if the type has none
    uint32_t created;               // Seconds since the Unix epoch, 0 = unknown
    uint32_t modified;
} FsNodeAttr;

typedef struct {
    uint32_t block_size;            // Allocation unit in bytes
    uint64_t total_size;            // Bytes the volume covers
} FsVolumeInfo;

struct FsType {
    const char* name;               // "basefs"; shown by device info
    uint8_t     mbr_type;           // MBR partition type it lives in, 0 if none

    // True if a device's first sector (sector_size bytes) starts a volume of
    // this type. Decides which type a device is mounted with.
    bool (*probe)(const uint8_t* sector0, uint32_t sector_size);

    //
    // Volume
    //

    // Bind the mount to `device` and open the volume on it, allocating
    // m->data on first use. Sets m->device always and m->is_open when the
    // volume is readable; an unformatted device still attaches (so it can
    // be formatted). False only when out of memory.
    bool (*attach)(struct FsMount* m, struct BlockDevice* device);

    // Forget the device and everything cached from it. The caller has synced.
    // m->device becomes NULL and m->is_open false; m->data is kept.
    void (*detach)(struct FsMount* m);

    // Free m->data (the mount is changing type). Called detached.
    void (*release)(struct FsMount* m);

    // Push pending changes to the device, its write cache included
    bool (*sync)(struct FsMount* m);

    bool     (*volume_info)(struct FsMount* m, FsVolumeInfo* out);
    uint64_t (*used_bytes)(struct FsMount* m);

    // Formatting. max_capacity: the most a volume of this type can cover on
    // `device`. format_check: whether capacity / block_size are acceptable.
    // format: write an empty volume on m->device (detached state is the
    // caller's: it re-attaches afterwards).
    uint64_t (*max_capacity)(struct BlockDevice* device);
    bool     (*format_check)(struct BlockDevice* device, uint64_t capacity, uint32_t block_size);
    bool     (*format)(struct FsMount* m, struct BlockDevice* device, uint64_t capacity, uint32_t block_size);

    //
    // Nodes (volume open)
    //

    uint64_t (*root)(struct FsMount* m);
    uint8_t  (*node_type)(struct FsMount* m, uint64_t node);       // FS_NODE_*

    // Directory holding `node`, FS_NODE_NULL for the root
    uint64_t (*parent)(struct FsMount* m, uint64_t node);

    uint64_t (*lookup)(struct FsMount* m, uint64_t dir, const char* name);
    uint32_t (*dir_count)(struct FsMount* m, uint64_t dir);

    // Entry `index` of `dir`. `node` and `name` may be NULL; a name that
    // does not fit name_size fails the call.
    bool (*dir_entry)(struct FsMount* m, uint64_t dir, uint32_t index,
                      uint64_t* node, char* name, size_t name_size);

    // New empty file or directory (node_type FS_NODE_FILE / _DIRECTORY) in
    // `dir`. The caller has checked the name is free. FS_NODE_NULL on failure.
    uint64_t (*create)(struct FsMount* m, uint64_t dir, const char* name,
                       uint8_t node_type, uint8_t permissions);

    // Unlink `node` from `dir` and free it; a directory goes with everything
    // inside it
    bool (*remove)(struct FsMount* m, uint64_t dir, uint64_t node);

    // Rename the entry for `node` in `dir`. Fails if the name is taken.
    bool (*rename)(struct FsMount* m, uint64_t dir, uint64_t node, const char* name);

    // Grow or shrink a file. Fails (file unchanged) on a directory or when
    // out of space.
    bool (*resize)(struct FsMount* m, uint64_t node, uint64_t size);

    bool (*get_attr)(struct FsMount* m, uint64_t node, FsNodeAttr* out);
    bool (*set_permissions)(struct FsMount* m, uint64_t node, uint8_t permissions);

    // Optional (NULL: the type cannot store them, and setting one fails)
    bool (*set_security)(struct FsMount* m, uint64_t node, uint32_t security);
    bool (*set_certificate)(struct FsMount* m, uint64_t node, uint32_t certificate);

    //
    // Open files
    //

    // Handle for reading and/or writing `node`. mode: VFS_OPEN_READ,
    // VFS_OPEN_WRITE and VFS_OPEN_APPEND (handle starts at the end).
    // NULL on failure.
    void*    (*open)(struct FsMount* m, uint64_t node, uint16_t mode);
    void     (*close)(void* file);
    uint32_t (*read)(void* file, void* buffer, uint32_t size);
    uint32_t (*write)(void* file, const void* buffer, uint32_t size);
    uint64_t (*seek)(void* file, uint64_t position);
    uint64_t (*tell)(void* file);
};

// Built-in types (listed in vfs_mount.c)
extern const struct FsType fs_type_basefs;

#endif
