#include <stdint.h>
#include <stdbool.h>
#include <kernel/malloc.h>

#include <kernel/knode.h>
#include <kernel/fs/fs_type.h>
#include <kernel/block/block.h>
#include <kernel/block/partition.h>

#include <kernel/vfs/vfs_internal.h>

#include <kernel/util/string.h>
#include <kernel/util/tok.h>
#include <kernel/util/list.h>
#include <kernel/util/timer.h>
#include <kernel/scheduler/scheduler.h>

//
// Internal helpers (caller holds the VFS lock)
//

_Static_assert(VFS_DEVICE_NAME_MAX == BLOCK_NAME_MAX, "VFS_DEVICE_NAME_MAX must match BLOCK_NAME_MAX");

struct FsMount* vfs_device_get_mount_context(const char* path) {
    // NULL unless the node is actually a mounted filesystem
    return vfs_knode_mount(resolve_path_to_mount_point(path));
}

struct FsMount* vfs_device_get_context(const char* path) {
    // Path resolution only enters an open device, so with any other one the
    // resolved address is the mount knode itself, not a filesystem node.
    // Handing that mount out made callers read the disk at a knode
    // address and treat whatever was there as a header.
    struct FsMount* mount = vfs_device_get_mount_context(path);
    return (mount != NULL && mount->is_open) ? mount : NULL;
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

// Total size of the volume, 0 when it is not open
static uint64_t vfs_mount_volume_size(struct FsMount* mount) {
    FsVolumeInfo volume;
    if (mount == NULL || !mount->is_open || !mount->type->volume_info(mount, &volume))
        return 0;
    return volume.total_size;
}

static uint64_t do_device_get_capacity(const char* path) {
    return vfs_mount_volume_size(vfs_device_get_mount_context(path));
}

static bool do_device_get_info(const char* path, VfsDeviceInfo* out) {
    memset(out, 0, sizeof(VfsDeviceInfo));
    
    struct FsMount* mount = vfs_device_get_mount_context(path);
    if (mount == NULL || mount->device == NULL)
        return false;   // Not on a mount, or the mount was detached
    
    struct BlockDevice* device = mount->device;
    size_t length = strnlen(device->name, BLOCK_NAME_MAX - 1);
    memcpy(out->device, device->name, length);
    out->device[length] = '\0';
    out->device_sector_size = device->sector_size;
    out->device_sectors     = device->sector_count;
    
    length = strnlen(mount->type->name, VFS_FS_NAME_MAX - 1);
    memcpy(out->filesystem, mount->type->name, length);
    out->filesystem[length] = '\0';
    
    FsVolumeInfo volume;
    if (mount->is_open && mount->type->volume_info(mount, &volume)) {
        out->formatted  = true;
        out->block_size = volume.block_size;
        out->total_size = volume.total_size;
    }
    return true;
}

// One step of the count; true when *used is known
static bool do_device_get_used(const char* path, uint64_t* used, uint32_t* percent) {
    *used = 0;
    if (percent) *percent = 100;
    struct FsMount* mount = vfs_device_get_context(path);
    if (!mount) return true;
    return mount->type->used_bytes(mount, used, percent);
}

static bool do_sync_all(void) {
    void* mnt = vfs_mnt_directory();
    if (mnt == NULL)
        return true;   // No mount directory: nothing to flush
    
    bool ok = true;
    uint32_t count = knode_get_reference_count(mnt);
    
    for (uint32_t i = 0; i < count; i++) {
        struct FsMount* mount = vfs_knode_mount(knode_get_reference(mnt, i));
        if (mount == NULL)
            continue;
        
        // An unformatted device was never written through, so it has
        // nothing pending; skip it rather than poke the hardware
        if (!mount->is_open)
            continue;
        
        if (!mount->type->sync(mount))
            ok = false;
    }
    
    return ok;
}

static bool vfs_mount_has_open_files(const struct FsMount* mount) {
    for (struct list_node* n = open_files_head; n != NULL; n = n->next) {
        OpenFileDescriptor* desc = (OpenFileDescriptor*)n->data;
        if (desc != NULL && desc->in_file_system && desc->mount == mount)
            return true;
    }
    return false;
}

// Zeroes for the full-format wipe. Static, so it is mapped and stays put
// while a DMA driver (AHCI) reads it.
#define VFS_WIPE_BUFFER_SIZE  4096
static uint8_t vfs_wipe_zeroes[VFS_WIPE_BUFFER_SIZE] __attribute__((aligned(16)));

// Zero every device sector the new filesystem will cover
static bool vfs_wipe_device(struct BlockDevice* device, uint64_t capacity, vfs_format_progress_fn progress) {
    uint32_t sector_size = device->sector_size;
    if (sector_size > VFS_WIPE_BUFFER_SIZE)
        return false;
    
    uint64_t total   = (capacity + sector_size - 1) / sector_size;
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

static VfsFormatResult do_device_format_context(struct FsMount* mount, const struct FsType* type,
                                                uint64_t capacity, uint32_t block_size, bool full_wipe,
                                                vfs_format_progress_fn progress) {
    if (mount == NULL || mount->device == NULL)
        return VFS_FORMAT_NOT_MOUNTED;
    
    struct BlockDevice* device = mount->device;
    if (type == NULL)
        type = mount->type;
    
    if (!type->format_check(device, capacity, block_size))
        return VFS_FORMAT_BAD_SIZE;
    
    // Open files hold addresses into the old layout: refuse rather than
    // leave them pointing into freshly formatted space
    if (vfs_mount_has_open_files(mount))
        return VFS_FORMAT_BUSY;
    
    // Commit what is pending on the old filesystem, then forget everything
    // the mount has cached about it (its buffers are about to be reused)
    if (mount->is_open)
        mount->type->sync(mount);
    mount->type->detach(mount);
    
    // Another filesystem: the old type's state goes, the new one binds to
    // the device (unformatted for it, so attach leaves is_open false)
    if (type != mount->type) {
        if (mount->data != NULL)
            mount->type->release(mount);
        mount->type = type;
        mount->data = NULL;
        if (!type->attach(mount, device)) {
            mount->device = device;     // Out of memory: keep it formattable
            return VFS_FORMAT_IO_ERROR;
        }
        type->detach(mount);
    }
    
    VfsFormatResult result = VFS_FORMAT_OK;
    
    if (full_wipe && !vfs_wipe_device(device, capacity, progress))
        result = VFS_FORMAT_IO_ERROR;
    
    if (result == VFS_FORMAT_OK && !type->format(mount, device, capacity, block_size))
        result = VFS_FORMAT_IO_ERROR;
    
    // Re-open even on failure, so the mount matches what is on disk now
    if (!type->attach(mount, device))
        mount->device = device;     // Out of memory: keep it formattable
    
    if (result == VFS_FORMAT_OK && (!mount->is_open || type->root(mount) == FS_NODE_NULL))
        result = VFS_FORMAT_IO_ERROR;
    
    return result;
}

static VfsCheckResult do_device_check(struct FsMount* mount, bool repair, VfsCheckReport* report) {
    if (mount->device == NULL)
        return VFS_CHECK_NOT_MOUNTED;
    if (!mount->is_open)
        return VFS_CHECK_UNFORMATTED;
    if (mount->type->check == NULL)
        return VFS_CHECK_UNSUPPORTED;

    // Open handles cache positions in the structures a repair may rewrite
    if (repair && vfs_mount_has_open_files(mount))
        return VFS_CHECK_BUSY;

    VfsCheckResult result = mount->type->check(mount, repair, report);

    if (repair && !mount->type->sync(mount) && result == VFS_CHECK_OK)
        result = VFS_CHECK_IO_ERROR;
    return result;
}

// Cut a mount loose from its disk: it reports as unformatted and every
// filesystem call on it becomes a no-op. The mount knode stays;
// vfs_mount_device re-attaches it by name.
static void vfs_mount_detach(struct FsMount* mount) {
    mount->type->detach(mount);
}

// The mount of the i-th entry of /mnt, or NULL if it is not a mount
static struct FsMount* vfs_mount_at(void* mnt, uint32_t index) {
    return vfs_knode_mount(knode_get_reference(mnt, index));
}

static bool vfs_mount_on_disk(const struct FsMount* mount, const struct BlockDevice* disk) {
    return mount->device != NULL &&
           (mount->device == disk || block_partition_parent(mount->device) == disk);
}

// Detach every mount on `disk` or on one of its partitions, after syncing
// it. Refuses (and changes nothing) while a file on any of them is open.
static VfsFormatResult do_release_disk(struct BlockDevice* disk) {
    void* mnt = vfs_mnt_directory();
    if (mnt == NULL)
        return VFS_FORMAT_OK;
    uint32_t count = knode_get_reference_count(mnt);
    
    for (uint32_t i = 0; i < count; i++) {
        struct FsMount* mount = vfs_mount_at(mnt, i);
        if (mount != NULL && vfs_mount_on_disk(mount, disk) && vfs_mount_has_open_files(mount))
            return VFS_FORMAT_BUSY;
    }
    
    for (uint32_t i = 0; i < count; i++) {
        struct FsMount* mount = vfs_mount_at(mnt, i);
        if (mount == NULL || !vfs_mount_on_disk(mount, disk))
            continue;
        if (mount->is_open)
            mount->type->sync(mount);
        vfs_mount_detach(mount);
    }
    return VFS_FORMAT_OK;
}

// True if a /mnt entry named `name` is `disk` itself or one of its
// partitions ("ahci0", "ahci0p1"; never "ahci01")
static bool vfs_name_on_disk(const char* name, const struct BlockDevice* disk) {
    size_t length = strnlen(disk->name, BLOCK_NAME_MAX);
    if (strncmp(name, disk->name, length) != 0)
        return false;
    if (name[length] == '\0')
        return true;
    if (name[length] != 'p' || name[length + 1] < '0' || name[length + 1] > '9')
        return false;
    for (size_t i = length + 2; name[i] != '\0'; i++) {
        if (name[i] < '0' || name[i] > '9')
            return false;
    }
    return true;
}

// Remove the /mnt entries of `disk` that a rescan left detached: the whole
// disk once it holds a partition table, partitions the new table no longer
// has. A detached mount no longer knows its device, so they go by name.
static void do_prune_disk(struct BlockDevice* disk) {
    void* mnt = vfs_mnt_directory();
    if (mnt == NULL)
        return;
    
    for (uint32_t i = 0; i < knode_get_reference_count(mnt); ) {
        void* knode = knode_get_reference(mnt, i);
        struct FsMount* mount = vfs_knode_mount(knode);
        char name[KNODE_NAME_LENGTH_MAX];
        if (mount != NULL)
            knode_get_name(knode, name);
        
        if (mount == NULL || mount->device != NULL || !vfs_name_on_disk(name, disk) ||
            vfs_mount_has_open_files(mount)) {
            i++;
            continue;
        }
        
        if (mount->data != NULL)
            mount->type->release(mount);
        knode_remove_reference(knode, mount);
        free(mount);
        destroy_knode(knode, mnt);  // Shifts the next entry into slot i
    }
}

static struct FsMount* vfs_find_device_mount(const struct BlockDevice* device) {
    void* mnt = vfs_mnt_directory();
    if (mnt == NULL)
        return NULL;
    uint32_t count = knode_get_reference_count(mnt);
    for (uint32_t i = 0; i < count; i++) {
        struct FsMount* mount = vfs_mount_at(mnt, i);
        if (mount != NULL && mount->device == device)
            return mount;
    }
    return NULL;
}

// Replace whatever a whole disk holds with an MBR whose single partition,
// of the mount's filesystem type, starts at `first_sector`. The whole-disk
// mount is detached afterwards (it would otherwise cover the new table, and
// a format through it would erase the table again); the partition mounts on
// the next boot.
static VfsFormatResult do_device_partition(const char* path, uint64_t first_sector) {
    struct FsMount* mount = vfs_device_get_mount_context(path);
    if (mount == NULL || mount->device == NULL)
        return VFS_FORMAT_NOT_MOUNTED;
    
    // Only whole disks get a table; a partition cannot hold another one
    if (block_partition_parent(mount->device) != NULL)
        return VFS_FORMAT_BAD_SIZE;
    if (mount->device->sector_size != MBR_SECTOR_SIZE || mount->type->mbr_types[0] == 0 ||
        first_sector == 0 || first_sector >= mount->device->sector_count)
        return VFS_FORMAT_BAD_SIZE;
    
    if (vfs_mount_has_open_files(mount))
        return VFS_FORMAT_BUSY;
    
    if (mount->is_open)
        mount->type->sync(mount);
    
    struct BlockDevice* disk = mount->device;
    uint8_t mbr_type = mount->type->mbr_types[0];
    
    // Detach first: from here on nothing can write through the old mount,
    // whether or not the table write below succeeds
    vfs_mount_detach(mount);
    
    if (!block_mbr_write_single(disk, mbr_type, first_sector))
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

bool vfs_device_get_used(const char* path, uint32_t budget_ms, uint64_t* used, uint32_t* percent) {
    uint64_t start = timer_get_ms();
    for (;;) {
        // The lock is taken for one step only: the mount is looked up again
        // each time, so a device unplugged in between just ends the count
        vfs_lock();
        bool done = do_device_get_used(path, used, percent);
        vfs_unlock();
        if (done || timer_get_ms() - start >= budget_ms)
            return done;
        thread_yield();     // Whoever waited for the lock runs now
    }
}

VfsFormatResult vfs_device_format_context(struct FsMount* mount, uint64_t capacity,
                                          uint32_t block_size, bool full_wipe,
                                          vfs_format_progress_fn progress) {
    vfs_lock();
    VfsFormatResult result = do_device_format_context(mount, NULL, capacity, block_size, full_wipe, progress);
    vfs_unlock();
    return result;
}

VfsFormatResult vfs_device_format_as(struct FsMount* mount, const struct FsType* type,
                                     uint64_t capacity, uint32_t block_size, bool full_wipe,
                                     vfs_format_progress_fn progress) {
    vfs_lock();
    VfsFormatResult result = do_device_format_context(mount, type, capacity, block_size, full_wipe, progress);
    vfs_unlock();
    return result;
}

uint64_t vfs_device_max_capacity(struct FsMount* mount) {
    if (mount == NULL)
        return 0;
    vfs_lock();
    uint64_t result = (mount->device != NULL) ? mount->type->max_capacity(mount->device) : 0;
    vfs_unlock();
    return result;
}

VfsCheckResult vfs_device_check(struct FsMount* mount, bool repair, VfsCheckReport* report) {
    if (mount == NULL || report == NULL)
        return VFS_CHECK_NOT_MOUNTED;

    vfs_check_problem_fn problem = report->problem;
    void* user = report->user;
    memset(report, 0, sizeof(VfsCheckReport));
    report->problem = problem;
    report->user    = user;

    vfs_lock();
    VfsCheckResult result = do_device_check(mount, repair, report);
    vfs_unlock();
    return result;
}

bool vfs_device_format(const char* path) {
    vfs_lock();
    struct FsMount* mount = vfs_device_get_mount_context(path);
    VfsFormatResult result = VFS_FORMAT_NOT_MOUNTED;
    if (mount != NULL && mount->device != NULL) {
        result = do_device_format_context(mount, NULL, mount->type->max_capacity(mount->device),
                                          mount->device->sector_size, false, NULL);
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

// The disk was unplugged: detach every mount on it or its partitions
// (nothing is synced, the disk is gone) and remove their /mnt entries. A
// mount with a file still open keeps its entry, detached, until it is
// closed; it reports as not mounted meanwhile.
VfsFormatResult vfs_device_remove_disk(struct BlockDevice* disk) {
    if (disk == NULL) return VFS_FORMAT_NOT_MOUNTED;
    vfs_lock();
    void* mnt = vfs_mnt_directory();
    if (mnt != NULL) {
        uint32_t count = knode_get_reference_count(mnt);
        for (uint32_t i = 0; i < count; i++) {
            struct FsMount* mount = vfs_mount_at(mnt, i);
            if (mount != NULL && vfs_mount_on_disk(mount, disk))
                vfs_mount_detach(mount);
        }
        do_prune_disk(disk);
    }
    vfs_unlock();
    return VFS_FORMAT_OK;
}

void vfs_device_prune_disk(struct BlockDevice* disk) {
    if (disk == NULL) return;
    vfs_lock();
    do_prune_disk(disk);
    vfs_unlock();
}

struct FsMount* vfs_device_context(struct BlockDevice* device) {
    if (device == NULL) return NULL;
    vfs_lock();
    struct FsMount* mount = vfs_find_device_mount(device);
    vfs_unlock();
    return mount;
}
