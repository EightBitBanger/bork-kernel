#include <stdint.h>
#include <stdbool.h>
#include <kernel/malloc.h>

#include <kernel/knode.h>
#include <kernel/fs/fs.h>
#include <kernel/block/block.h>
#include <kernel/block/partition.h>

#include <kernel/vfs/vfs_internal.h>

#include <kernel/util/string.h>
#include <kernel/util/tok.h>
#include <kernel/util/list.h>

//
// Internal helpers (caller holds the VFS lock)
//

_Static_assert(VFS_DEVICE_NAME_MAX == BLOCK_NAME_MAX, "VFS_DEVICE_NAME_MAX must match BLOCK_NAME_MAX");

struct FSDeviceContext* vfs_device_get_mount_context(const char* path) {
    uint32_t address = resolve_path_to_mount_point(path);
    if (address == 0xFFFFFFFF || address == 0) 
        return NULL;
    if (knode_check_is_valid(address) == 0) 
        return NULL;
    
    // Check if the node is actually a mounted filesystem
    uint8_t flags = kmalloc_get_flags(address);
    if ((flags & KMALLOC_FLAG_MOUNT) == 0) {
        return NULL;
    }
    
    uint32_t ctx = knode_get_reference(address, 1);
    if (ctx == 0 || ctx == KMALLOC_NULL)
        return NULL;
    return (struct FSDeviceContext*)ctx;
}

struct FSDeviceContext* vfs_device_get_context(const char* path) {
    // Path resolution only enters an open device, so with any other one the
    // resolved address is the mount knode itself, not a basefs address.
    // Handing that context out made callers read the disk at a knode
    // address and treat whatever was there as a header.
    struct FSDeviceContext* ctx = vfs_device_get_mount_context(path);
    return (ctx != NULL && ctx->is_open) ? ctx : NULL;
}

uint8_t* vfs_device_get_block(const char* path) {
    uint32_t address = resolve_path_to_mount_point(path);
    if (address == 0xFFFFFFFF || address == 0) 
        return NULL;
    if (knode_check_is_valid(address) == 0) 
        return NULL;
    
    // Check if the node is actually a mounted filesystem
    uint8_t flags = kmalloc_get_flags(address);
    if ((flags & KMALLOC_FLAG_MOUNT) == 0) {
        return NULL;
    }
    
    return (uint8_t*)knode_get_reference(address, 0);
}

// The returned pointer is only safe to use while the caller holds the VFS
// lock. The lock taken here (recursively) just guards the walk itself.
OpenFileDescriptor* vfs_file_find_open(File id) {
    if (id == VFS_INVALID_FILE) return NULL;
    
    vfs_lock();
    OpenFileDescriptor* found = NULL;
    struct list_node* current = open_files_head;
    while (current != NULL) {
        OpenFileDescriptor* desc = (OpenFileDescriptor*)current->data;
        if (desc->id == id) {
            found = desc;
            break;
        }
        current = current->next;
    }
    vfs_unlock();
    return found;
}

//
// Implementations (caller holds the VFS lock)
//

static uint64_t do_device_get_capacity(const char* path) {
    uint32_t address = resolve_path_to_mount_point(path);
    if (address == 0xFFFFFFFF || address == 0) 
        return 0;
    if (knode_check_is_valid(address) == 0) 
        return 0;
    
    struct FSDeviceContext* device_context = (struct FSDeviceContext*)knode_get_reference(address, 1);
    if (!device_context) return 0;

    return device_context->pool_size;
}

static bool do_device_get_info(const char* path, VfsDeviceInfo* out) {
    memset(out, 0, sizeof(VfsDeviceInfo));
    
    struct FSDeviceContext* ctx = vfs_device_get_mount_context(path);
    if (ctx == NULL || ctx->device == NULL)
        return false;   // Not on a mount, or the mount was detached
    
    struct BlockDevice* device = ctx->device;
    size_t length = strnlen(device->name, BLOCK_NAME_MAX - 1);
    memcpy(out->device, device->name, length);
    out->device[length] = '\0';
    out->device_sector_size = device->sector_size;
    out->device_sectors     = device->sector_count;
    
    struct FSPartitionBlock partition;
    if (ctx->is_open && fs_device_get_partition(ctx, &partition) == 0) {
        out->formatted  = true;
        out->block_size = partition.sector_size;
        out->total_size = partition.total_size;
    }
    return true;
}

static uint64_t do_device_get_used(const char* path) {
    struct FSDeviceContext* device_context = vfs_device_get_context(path);
    if (!device_context) return 0;
    return fs_get_used_bytes(device_context);
}

static bool do_sync_all(void) {
    uint32_t root = knode_get_root();
    uint32_t mnt  = knode_find_by_name(root, "mnt");
    if (mnt == KNODE_NULL || mnt == 0)
        return true;   // No mount directory: nothing to flush
    
    bool ok = true;
    uint32_t count = knode_get_reference_count(mnt);
    
    for (uint32_t i = 0; i < count; i++) {
        uint32_t mount = knode_get_reference(mnt, i);
        if (mount == KNODE_NULL || mount == 0)
            continue;
        if ((kmalloc_get_flags(mount) & KMALLOC_FLAG_MOUNT) == 0)
            continue;
        
        // Reference 1 of a mount knode is its FSDeviceContext (see pci.c)
        uint32_t ctx_ref = knode_get_reference(mount, 1);
        if (ctx_ref == KMALLOC_NULL || ctx_ref == 0)
            continue;
        
        struct FSDeviceContext* ctx = (struct FSDeviceContext*)ctx_ref;
        
        // An unformatted device was never written through, so it has
        // nothing pending; skip it rather than poke the hardware
        if (!ctx->is_open)
            continue;
        
        if (!fs_device_sync(ctx))
            ok = false;
    }
    
    return ok;
}

static bool vfs_context_has_open_files(const struct FSDeviceContext* ctx) {
    for (struct list_node* n = open_files_head; n != NULL; n = n->next) {
        OpenFileDescriptor* desc = (OpenFileDescriptor*)n->data;
        if (desc != NULL && desc->ctx == ctx)
            return true;
    }
    return false;
}

// Zeroes for the full-format wipe. Static, so it is mapped and stays put
// while a DMA driver (AHCI) reads it.
#define VFS_WIPE_BUFFER_SIZE  4096
static uint8_t vfs_wipe_zeroes[VFS_WIPE_BUFFER_SIZE] __attribute__((aligned(16)));

// Zero every device sector the new filesystem will cover
static bool vfs_wipe_device(struct BlockDevice* device, uint32_t capacity, vfs_format_progress_fn progress) {
    uint32_t sector_size = device->sector_size;
    if (sector_size > VFS_WIPE_BUFFER_SIZE)
        return false;
    
    uint64_t total   = ((uint64_t)capacity + sector_size - 1) / sector_size;
    uint32_t chunk   = VFS_WIPE_BUFFER_SIZE / sector_size;
    uint32_t percent = 0;
    
    for (uint64_t lba = 0; lba < total; ) {
        uint32_t count = (total - lba < chunk) ? (uint32_t)(total - lba) : chunk;
        if (!block_write(device, lba, count, vfs_wipe_zeroes))
            return false;
        lba += count;
        
        uint32_t now = (uint32_t)((lba * 100) / total);
        if (progress != NULL && now != percent) {
            percent = now;
            progress(percent);
        }
    }
    return true;
}

static VfsFormatResult do_device_format_context(struct FSDeviceContext* ctx, uint32_t capacity,
                                                uint32_t block_size, bool full_wipe,
                                                vfs_format_progress_fn progress) {
    if (ctx == NULL || ctx->device == NULL || ctx->device_address == 0)
        return VFS_FORMAT_NOT_MOUNTED;
    
    struct BlockDevice* device = ctx->device;
    uint32_t window = ctx->device_address;
    
    if (block_size < FS_MIN_SECTOR_SIZE || (block_size % 4U) != 0 ||
        capacity < block_size || capacity > fs_device_max_capacity(device))
        return VFS_FORMAT_BAD_SIZE;
    
    // Open files hold addresses into the old layout: refuse rather than
    // leave them pointing into freshly formatted space
    if (vfs_context_has_open_files(ctx))
        return VFS_FORMAT_BUSY;
    
    // Commit what is pending on the old filesystem, then forget everything
    // the context has cached about it (the window is about to be reused)
    if (ctx->is_open)
        fs_device_sync(ctx);
    ctx->is_open      = false;
    ctx->sector_frame = 0xFFFFFFFF;
    ctx->sector_dirty = false;
    ctx->frame_offset = FS_INVALID_FRAME;
    ctx->frame_dirty  = false;
    
    VfsFormatResult result = VFS_FORMAT_OK;
    
    if (full_wipe && !vfs_wipe_device(device, capacity, progress))
        result = VFS_FORMAT_IO_ERROR;
    
    if (result == VFS_FORMAT_OK && !fs_device_format(device, window, capacity, block_size))
        result = VFS_FORMAT_IO_ERROR;
    
    // Re-open even on failure, so the context matches what is on disk now
    struct FSPartitionBlock part;
    *ctx = fs_device_open(device, window, &part);
    
    if (result == VFS_FORMAT_OK && (!ctx->is_open || part.root_directory == FS_NULL))
        result = VFS_FORMAT_IO_ERROR;
    
    return result;
}

// Cut a mount loose from its disk: it reports as unformatted and every
// basefs call on it becomes a no-op (fs_io_ready fails without a device).
// The mount knode stays; pci_mount_block_device re-attaches it by name.
static void vfs_context_detach(struct FSDeviceContext* ctx) {
    ctx->is_open      = false;
    ctx->device       = NULL;
    ctx->pool_size    = 0;
    ctx->block_count  = 0;
    ctx->sector_frame = 0xFFFFFFFF;
    ctx->sector_dirty = false;
    ctx->frame_offset = FS_INVALID_FRAME;
    ctx->frame_dirty  = false;
}

// The context of the i-th entry of /mnt, or NULL if it is not a mount
static struct FSDeviceContext* vfs_mount_context(uint32_t mnt, uint32_t index) {
    uint32_t mount = knode_get_reference(mnt, index);
    if (mount == KNODE_NULL || mount == 0 || mount == KMALLOC_NULL)
        return NULL;
    if ((kmalloc_get_flags(mount) & KMALLOC_FLAG_MOUNT) == 0)
        return NULL;
    
    // Reference 1 of a mount knode is its FSDeviceContext (see pci.c)
    uint32_t ctx_ref = knode_get_reference(mount, 1);
    if (ctx_ref == KMALLOC_NULL || ctx_ref == 0)
        return NULL;
    return (struct FSDeviceContext*)ctx_ref;
}

static uint32_t vfs_mnt_directory(void) {
    uint32_t mnt = knode_find_by_name(knode_get_root(), "mnt");
    return (mnt == KNODE_NULL || mnt == KMALLOC_NULL) ? 0 : mnt;
}

static bool vfs_context_on_disk(const struct FSDeviceContext* ctx, const struct BlockDevice* disk) {
    return ctx->device != NULL &&
           (ctx->device == disk || block_partition_parent(ctx->device) == disk);
}

// Detach every mount on `disk` or on one of its partitions, after syncing
// it. Refuses (and changes nothing) while a file on any of them is open.
static VfsFormatResult do_release_disk(struct BlockDevice* disk) {
    uint32_t mnt = vfs_mnt_directory();
    if (mnt == 0)
        return VFS_FORMAT_OK;
    uint32_t count = knode_get_reference_count(mnt);
    
    for (uint32_t i = 0; i < count; i++) {
        struct FSDeviceContext* ctx = vfs_mount_context(mnt, i);
        if (ctx != NULL && vfs_context_on_disk(ctx, disk) && vfs_context_has_open_files(ctx))
            return VFS_FORMAT_BUSY;
    }
    
    for (uint32_t i = 0; i < count; i++) {
        struct FSDeviceContext* ctx = vfs_mount_context(mnt, i);
        if (ctx == NULL || !vfs_context_on_disk(ctx, disk))
            continue;
        if (ctx->is_open)
            fs_device_sync(ctx);
        vfs_context_detach(ctx);
    }
    return VFS_FORMAT_OK;
}

static struct FSDeviceContext* vfs_find_device_context(const struct BlockDevice* device) {
    uint32_t mnt = vfs_mnt_directory();
    if (mnt == 0)
        return NULL;
    uint32_t count = knode_get_reference_count(mnt);
    for (uint32_t i = 0; i < count; i++) {
        struct FSDeviceContext* ctx = vfs_mount_context(mnt, i);
        if (ctx != NULL && ctx->device == device)
            return ctx;
    }
    return NULL;
}

// Replace whatever a whole disk holds with an MBR whose single basefs
// partition starts at `first_sector`. The whole-disk mount is detached
// afterwards (it would otherwise cover the new table, and a format through
// it would erase the table again); the partition mounts on the next boot.
static VfsFormatResult do_device_partition(const char* path, uint64_t first_sector) {
    struct FSDeviceContext* ctx = vfs_device_get_mount_context(path);
    if (ctx == NULL || ctx->device == NULL)
        return VFS_FORMAT_NOT_MOUNTED;
    
    // Only whole disks get a table; a partition cannot hold another one
    if (block_partition_parent(ctx->device) != NULL)
        return VFS_FORMAT_BAD_SIZE;
    if (ctx->device->sector_size != MBR_SECTOR_SIZE ||
        first_sector == 0 || first_sector >= ctx->device->sector_count)
        return VFS_FORMAT_BAD_SIZE;
    
    if (vfs_context_has_open_files(ctx))
        return VFS_FORMAT_BUSY;
    
    if (ctx->is_open)
        fs_device_sync(ctx);
    
    struct BlockDevice* disk = ctx->device;
    
    // Detach first: from here on nothing can write through the old mount,
    // whether or not the table write below succeeds
    vfs_context_detach(ctx);
    
    if (!block_mbr_write_single(disk, MBR_TYPE_BASEFS, first_sector))
        return VFS_FORMAT_IO_ERROR;
    return VFS_FORMAT_OK;
}

//
// Public API
//

uint64_t vfs_device_get_capacity(const char* path) {
    vfs_lock();
    uint64_t result = do_device_get_capacity(path);
    vfs_unlock();
    return result;
}

bool vfs_device_get_info(const char* path, VfsDeviceInfo* out) {
    if (out == NULL) return false;
    vfs_lock();
    bool result = do_device_get_info(path, out);
    vfs_unlock();
    return result;
}

uint64_t vfs_device_get_used(const char* path) {
    vfs_lock();
    uint64_t result = do_device_get_used(path);
    vfs_unlock();
    return result;
}

VfsFormatResult vfs_device_format_context(struct FSDeviceContext* ctx, uint32_t capacity,
                                          uint32_t block_size, bool full_wipe,
                                          vfs_format_progress_fn progress) {
    vfs_lock();
    VfsFormatResult result = do_device_format_context(ctx, capacity, block_size, full_wipe, progress);
    vfs_unlock();
    return result;
}

bool vfs_device_format(const char* path) {
    vfs_lock();
    struct FSDeviceContext* ctx = vfs_device_get_mount_context(path);
    VfsFormatResult result = VFS_FORMAT_NOT_MOUNTED;
    if (ctx != NULL && ctx->device != NULL) {
        result = do_device_format_context(ctx, fs_device_max_capacity(ctx->device),
                                          ctx->device->sector_size, false, NULL);
    }
    vfs_unlock();
    return result == VFS_FORMAT_OK;
}

bool vfs_sync_all(void) {
    vfs_lock();
    bool result = do_sync_all();
    vfs_unlock();
    return result;
}

VfsFormatResult vfs_device_partition(const char* path, uint64_t first_sector) {
    vfs_lock();
    VfsFormatResult result = do_device_partition(path, first_sector);
    vfs_unlock();
    return result;
}

VfsFormatResult vfs_device_release_disk(struct BlockDevice* disk) {
    if (disk == NULL) return VFS_FORMAT_NOT_MOUNTED;
    vfs_lock();
    VfsFormatResult result = do_release_disk(disk);
    vfs_unlock();
    return result;
}

struct FSDeviceContext* vfs_device_context(struct BlockDevice* device) {
    if (device == NULL) return NULL;
    vfs_lock();
    struct FSDeviceContext* ctx = vfs_find_device_context(device);
    vfs_unlock();
    return ctx;
}

