// Mounts and filesystem types
//
// Every block device the storage scan hands over becomes /mnt/<device>: a
// knode whose only reference is a struct FsMount. The FsMount names the
// filesystem type that drives the volume (fs/fs_type.h), picked by asking
// each known type whether the device's first sector is one of its volumes.
// A device no type recognizes is mounted with the first type in the list,
// unformatted, so it can be formatted.

#include <stdint.h>
#include <stdbool.h>
#include <kernel/malloc.h>

#include <kernel/knode.h>
#include <kernel/block/block.h>
#include <kernel/fs/fs_type.h>

#include <kernel/vfs/vfs.h>
#include <kernel/vfs/vfs_internal.h>

#include <kernel/util/string.h>

// Known filesystems. The first one formats blank devices.
static const struct FsType* const vfs_fs_types[] = {
    &fs_type_basefs,
    &fs_type_fat,
};

#define VFS_FS_TYPE_COUNT   (sizeof(vfs_fs_types) / sizeof(vfs_fs_types[0]))

// A device's first sector while its type is decided. Static, so DMA
// drivers can reach it; guarded by the VFS lock.
#define VFS_PROBE_BUFFER_SIZE   4096
static uint8_t vfs_probe_buffer[VFS_PROBE_BUFFER_SIZE] __attribute__((aligned(16)));

//
// Types
//

static const struct FsType* vfs_fs_type_of(const uint8_t* sector0, uint32_t sector_size) {
    for (uint32_t i = 0; i < VFS_FS_TYPE_COUNT; i++) {
        if (vfs_fs_types[i]->probe(sector0, sector_size))
            return vfs_fs_types[i];
    }
    return NULL;
}

bool vfs_fs_probe(const uint8_t* sector0, uint32_t sector_size) {
    return sector0 != NULL && vfs_fs_type_of(sector0, sector_size) != NULL;
}

bool fs_type_claims_mbr(const struct FsType* type, uint8_t mbr_type) {
    if (type == NULL || mbr_type == 0)
        return false;
    for (uint32_t i = 0; i < FS_MBR_TYPES_MAX && type->mbr_types[i] != 0; i++) {
        if (type->mbr_types[i] == mbr_type)
            return true;
    }
    return false;
}

bool vfs_fs_claims_partition(uint8_t mbr_type) {
    for (uint32_t i = 0; i < VFS_FS_TYPE_COUNT; i++) {
        if (fs_type_claims_mbr(vfs_fs_types[i], mbr_type))
            return true;
    }
    return false;
}

const struct FsType* vfs_fs_type_by_name(const char* name) {
    if (name == NULL || name[0] == '\0')
        return NULL;
    for (uint32_t i = 0; i < VFS_FS_TYPE_COUNT; i++) {
        const struct FsType* type = vfs_fs_types[i];
        if (strcmp(type->name, name) == 0 || (type->alias != NULL && strcmp(type->alias, name) == 0))
            return type;
    }
    return NULL;
}

// The type a device holds, or the default type when none recognizes it
// (blank, unreadable, or a sector too big to probe). Caller holds the lock.
static const struct FsType* vfs_device_fs_type(struct BlockDevice* device) {
    if (device->sector_size <= VFS_PROBE_BUFFER_SIZE &&
        block_read(device, 0, 1, vfs_probe_buffer)) {
        const struct FsType* type = vfs_fs_type_of(vfs_probe_buffer, device->sector_size);
        if (type != NULL)
            return type;
    }
    return vfs_fs_types[0];
}

//
// Mount knodes (caller holds the VFS lock)
//

void* vfs_mnt_directory(void) {
    return knode_find_by_name(knode_get_root(), "mnt");
}

struct FsMount* vfs_knode_mount(void* knode) {
    if (knode == NULL || !knode_check_is_valid(knode))
        return NULL;
    if ((kmalloc_get_flags(knode) & KMALLOC_FLAG_MOUNT) == 0)
        return NULL;
    return knode_get_reference(knode, 0);
}

// Open `device` in `mount`, switching the mount to whatever type the device
// holds now. False when out of memory.
static bool vfs_mount_attach(struct FsMount* mount, struct BlockDevice* device) {
    const struct FsType* type = vfs_device_fs_type(device);
    if (mount->type != type) {
        if (mount->type != NULL && mount->data != NULL)
            mount->type->release(mount);
        mount->type = type;
        mount->data = NULL;
    }
    return type->attach(mount, device);
}

static struct FsMount* do_mount_device(struct BlockDevice* device, void* mnt_dir, bool* remounted) {
    *remounted = false;

    // Mounted before and detached since (the disk was rewritten by "boot
    // install"): open the device again in the same knode instead of adding
    // a second /mnt entry with the same name
    void* existing = knode_find_by_name(mnt_dir, device->name);
    struct FsMount* mount = vfs_knode_mount(existing);
    if (mount != NULL) {
        *remounted = true;
        if (mount->device == NULL || mount->device == device)
            vfs_mount_attach(mount, device);
        return mount;
    }

    mount = malloc(sizeof(struct FsMount));
    if (mount == NULL)
        return NULL;
    memset(mount, 0, sizeof(struct FsMount));

    if (!vfs_mount_attach(mount, device)) {
        if (mount->data != NULL)
            mount->type->release(mount);
        free(mount);
        return NULL;
    }

    void* knode = create_knode(device->name, mnt_dir);
    if (knode == NULL) {
        mount->type->release(mount);
        free(mount);
        return NULL;
    }
    kmalloc_set_flags(knode, (KMALLOC_FLAG_DIRECTORY | KMALLOC_FLAG_MOUNT));
    knode_add_reference(knode, mount);
    return mount;
}

//
// Public API
//

struct FsMount* vfs_mount_device(struct BlockDevice* device, void* mnt_dir, bool* remounted) {
    bool again = false;
    struct FsMount* mount = NULL;
    if (device != NULL && mnt_dir != NULL) {
        vfs_lock();
        mount = do_mount_device(device, mnt_dir, &again);
        vfs_unlock();
    }
    if (remounted != NULL)
        *remounted = again;
    return mount;
}

const char* vfs_mount_fs_name(const struct FsMount* mount) {
    return (mount != NULL && mount->type != NULL) ? mount->type->name : "";
}

const struct FsType* vfs_fs_default_type(void) {
    return vfs_fs_types[0];
}
