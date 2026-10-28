#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <kernel/fs/format.h>
#include <kernel/fs/fs_type.h>
#include <kernel/kernel.h>
#include <kernel/mutex.h>
#include <kernel/block/block.h>
#include <kernel/block/partition.h>
#include <kernel/vfs/vfs.h>
#include <kernel/arch/x86/bus/pci/pci.h>
#include <kernel/util/string.h>

#define FORMAT_DEFAULT_BLOCK_SIZE   512U

// Sectors a quick whole-disk wipe zeroes at the front of the disk (boot
// sector, stage 2 and kernel) and at the head of each partition (where a
// filesystem keeps its headers)
#define FORMAT_QUICK_BOOT_SECTORS   VFS_PARTITION_START_BOOT
#define FORMAT_QUICK_HEAD_SECTORS   64ULL

// One format at a time, whoever asks. Zeroed is unlocked (mutex_init).
static mutex_t format_lock;

// Sector 0 while a quick wipe reads the partition table. Static, so DMA
// drivers can reach it; guarded by format_lock.
static uint8_t format_sector0[MBR_SECTOR_SIZE] __attribute__((aligned(16)));

static void format_mount_path(const char* device_name, char out[8 + BLOCK_NAME_MAX]) {
    strcpy(out, "/mnt/");
    strcat(out, device_name);
}

// True when `device` holds the system folders (home, "/mnt/<device>/sys"),
// is the disk the home partition is on, or is a partition of a home disk.
// Takes the VFS lock (kernel_get_local_paths).
static bool format_is_system(struct BlockDevice* device) {
    struct LocalPaths paths;
    kernel_get_local_paths(&paths);

    const char* home = paths.home;
    if (home[0] == '\0')
        return false;       // No system device claimed yet: nothing to protect

    // Anything but "/mnt/<name>[/...]" is unexpected: protect every device
    // rather than guess
    if (strncmp(home, "/mnt/", 5) != 0)
        return true;
    char name[BLOCK_NAME_MAX];
    size_t length = 0;
    for (const char* p = home + 5; *p != '\0' && *p != '/'; p++) {
        if (length + 1 >= sizeof(name))
            return true;
        name[length++] = *p;
    }
    name[length] = '\0';

    struct BlockDevice* home_device = block_find(name);
    if (home_device == NULL)
        return false;       // Home is gone (unplugged): no device holds it
    if (home_device == device)
        return true;
    if (block_partition_parent(home_device) == device)
        return true;        // The disk the home partition is on
    return block_partition_parent(device) == home_device;
}

FormatResult format_plan(const char* device_name, const FormatRequest* request, FormatPlan* plan) {
    memset(plan, 0, sizeof(FormatPlan));
    if (device_name == NULL || request == NULL)
        return FORMAT_NO_DEVICE;

    struct BlockDevice* device = block_find(device_name);
    if (device == NULL)
        return FORMAT_NO_DEVICE;

    strncpy(plan->device, device->name, BLOCK_NAME_MAX - 1);
    plan->device[BLOCK_NAME_MAX - 1] = '\0';
    plan->whole_disk         = block_partition_parent(device) == NULL;
    plan->quick              = request->quick;
    plan->device_sector_size = device->sector_size;
    plan->device_bytes       = block_capacity_bytes(device);
    plan->system             = format_is_system(device);

    // A whole disk is wiped and remounted, whatever it holds now; a
    // partition is formatted through its mount
    struct FsMount* mount = vfs_device_context(device);
    if (mount == NULL && !plan->whole_disk)
        return FORMAT_NOT_MOUNTED;

    char mount_path[8 + BLOCK_NAME_MAX];
    format_mount_path(plan->device, mount_path);
    VfsDeviceInfo existing;
    memset(&existing, 0, sizeof(existing));
    if (mount != NULL && vfs_device_get_info(mount_path, &existing) && existing.formatted)
        plan->formatted = true;

    // The filesystem it is driven as now: a blank disk comes back with the
    // default one (the wipe leaves nothing to probe), a partition keeps its
    // own. The request may name another; the mount is switched to it.
    bool named = request->filesystem != NULL && request->filesystem[0] != '\0';
    vfs_lock();
    const struct FsType* current = NULL;
    if (plan->whole_disk)
        current = vfs_fs_default_type();
    else if (mount->device == device)
        current = mount->type;
    const struct FsType* type = named ? vfs_fs_type_by_name(request->filesystem) : current;
    uint64_t max_capacity = (type != NULL && current != NULL) ? type->max_capacity(device) : 0;
    vfs_unlock();
    if (current == NULL)
        return FORMAT_NOT_MOUNTED;      // The mount was detached meanwhile

    // Policy before the request's details: the system device is refused
    // whatever is asked of it
    if (plan->system && !request->allow_system)
        return FORMAT_SYSTEM_DEVICE;
    if (type == NULL)
        return FORMAT_BAD_FILESYSTEM;

    strncpy(plan->filesystem, type->name, FORMAT_FS_NAME_MAX - 1);
    plan->filesystem[FORMAT_FS_NAME_MAX - 1] = '\0';
    plan->max_capacity = max_capacity;

    // Defaults: keep the current layout when the filesystem stays the same,
    // or use the whole device. A whole disk always gets all of it.
    bool keep = plan->formatted && type == current && strcmp(existing.filesystem, type->name) == 0;
    plan->capacity = request->capacity;
    if (plan->capacity == 0)
        plan->capacity = (keep && !plan->whole_disk && existing.total_size > 0)
                       ? existing.total_size : max_capacity;
    plan->block_size = request->block_size;
    if (plan->block_size == 0) {
        if (keep && existing.block_size > 0)
            plan->block_size = existing.block_size;
        else if (type->default_block_size != NULL)
            plan->block_size = type->default_block_size(device, plan->capacity);
        else
            plan->block_size = FORMAT_DEFAULT_BLOCK_SIZE;
    }

    if (plan->capacity > max_capacity)
        return FORMAT_TOO_LARGE;

    // A size that is not a whole number of blocks would leave a partial
    // block at the end that the filesystem can never allocate
    plan->capacity -= plan->capacity % plan->block_size;

    // The filesystem's own rules, so a request it would refuse fails here
    // and not after a whole disk has been wiped. The largest volume of that
    // block size tells a bad block size from a bad size.
    vfs_lock();
    bool size_ok  = type->format_check(device, plan->capacity, plan->block_size);
    bool block_ok = size_ok || type->format_check(device, max_capacity - max_capacity % plan->block_size,
                                                  plan->block_size);
    vfs_unlock();
    if (!block_ok)
        return FORMAT_BAD_BLOCK_SIZE;
    if (!size_ok)
        return FORMAT_TOO_SMALL;
    return FORMAT_OK;
}

static bool format_zero_head(struct BlockDevice* disk, uint64_t lba, uint64_t count) {
    if (lba >= disk->sector_count)
        return true;
    if (count > disk->sector_count - lba)
        count = disk->sector_count - lba;
    return block_zero(disk, lba, count, NULL);
}

// Erase `disk` from sector 0 on, after releasing everything mounted from it
// and its partitions, then mount it again (blank, as a whole). A full wipe
// zeroes every sector; a quick one the boot area, the heads of the
// partitions in its table and the last sectors.
static FormatResult format_wipe_disk(struct BlockDevice* disk, bool quick, bool quiet,
                                     void (*progress)(uint32_t)) {
    if (vfs_device_release_disk(disk) == VFS_FORMAT_BUSY)
        return FORMAT_BUSY;
    block_partition_drop_all(disk);

    bool ok = true;
    if (quick) {
        struct MbrPartition table[MBR_PARTITION_COUNT];
        uint8_t entries = 0;
        if (disk->sector_size == MBR_SECTOR_SIZE && block_read(disk, 0, 1, format_sector0) &&
            block_mbr_parse(format_sector0, disk->sector_count, table, &entries)) {
            for (uint8_t i = 0; i < entries && ok; i++)
                ok = format_zero_head(disk, table[i].start, FORMAT_QUICK_HEAD_SECTORS);
        }
        uint64_t tail = disk->sector_count > FORMAT_QUICK_HEAD_SECTORS
                      ? disk->sector_count - FORMAT_QUICK_HEAD_SECTORS : 0;
        ok = ok && format_zero_head(disk, tail, FORMAT_QUICK_HEAD_SECTORS);
        ok = ok && format_zero_head(disk, 0, FORMAT_QUICK_BOOT_SECTORS);
    } else {
        ok = block_zero(disk, 0, disk->sector_count, progress);
    }
    ok = ok && block_flush(disk);

    // Mounted again either way, so /mnt shows what is on the disk now.
    // Sector 0 is blank after a good wipe, so the scan mounts it whole.
    if (quiet)
        pci_mount_storage_quiet(disk);
    else
        pci_mount_storage(disk);
    return ok ? FORMAT_OK : FORMAT_WIPE_FAILED;
}

// Make the MBR entry of partition `device` one of `type`'s, if it is not
static void format_set_partition_type(struct BlockDevice* device, const struct FsType* type) {
    struct BlockDevice* disk = block_partition_parent(device);
    if (disk == NULL || type->mbr_types[0] == 0 || disk->sector_size != MBR_SECTOR_SIZE)
        return;

    struct MbrPartition table[MBR_PARTITION_COUNT];
    uint8_t entries = 0;
    if (!block_read(disk, 0, 1, format_sector0) ||
        !block_mbr_parse(format_sector0, disk->sector_count, table, &entries))
        return;

    uint64_t start = block_partition_start(device);
    for (uint8_t i = 0; i < entries; i++) {
        if (table[i].start != start)
            continue;
        if (fs_type_claims_mbr(type, table[i].type))
            return;
        table[i].type = type->mbr_types[0];
        block_mbr_write(disk, table, entries, true);
        return;
    }
}

static FormatResult format_execute(FormatPlan* plan, const FormatRequest* request,
                                   void (*progress)(uint32_t)) {
    struct BlockDevice* device = block_find(plan->device);
    if (device == NULL)
        return FORMAT_NO_DEVICE;

    // Whole disk: everything goes, from sector 0, before the volume is
    // written. It comes back mounted as a blank whole disk.
    if (plan->whole_disk) {
        FormatResult wiped = format_wipe_disk(device, request->quick, request->quiet, progress);
        if (wiped != FORMAT_OK)
            return wiped;
    }

    struct FsMount* mount = vfs_device_context(device);
    if (mount == NULL)
        return plan->whole_disk ? FORMAT_IO_ERROR : FORMAT_NOT_MOUNTED;

    // The plan's checks were made against this filesystem
    const struct FsType* type = vfs_fs_type_by_name(plan->filesystem);
    if (type == NULL)
        return FORMAT_IO_ERROR;

    // Wipe (a partition, unless quick), write the filesystem's structures
    // and re-open the mount, all under the VFS lock
    VfsFormatResult result = vfs_device_format_as(mount, type, plan->capacity, plan->block_size,
                                                  !request->quick && !plan->whole_disk, progress);
    switch (result) {
        case VFS_FORMAT_OK:          break;
        case VFS_FORMAT_NOT_MOUNTED: return FORMAT_NOT_MOUNTED;
        case VFS_FORMAT_BUSY:        return FORMAT_BUSY;
        case VFS_FORMAT_BAD_SIZE:    return FORMAT_TOO_LARGE;
        default:                     return FORMAT_IO_ERROR;
    }

    // The new root must read back
    char mount_path[8 + BLOCK_NAME_MAX];
    format_mount_path(plan->device, mount_path);
    VfsDeviceInfo formatted;
    if (!vfs_device_get_info(mount_path, &formatted) || !formatted.formatted)
        return FORMAT_IO_ERROR;

    plan->formatted = true;

    // A partition's table entry says what it holds, for other systems
    // (Windows will not mount a FAT volume in a basefs entry). Best effort:
    // the volume is good either way.
    if (!plan->whole_disk)
        format_set_partition_type(device, type);
    return FORMAT_OK;
}

FormatResult format_run(const char* device_name, const FormatRequest* request, FormatPlan* plan,
                        void (*progress)(uint32_t percent)) {
    FormatPlan local;
    if (plan == NULL)
        plan = &local;

    if (!format_lock_try()) {
        memset(plan, 0, sizeof(FormatPlan));
        return FORMAT_IN_PROGRESS;
    }

    FormatResult result = format_plan(device_name, request, plan);
    if (result == FORMAT_OK) {
        if (request->started != NULL)
            request->started(plan);
        result = format_execute(plan, request, progress);
    }

    format_lock_release();
    return result;
}

bool format_lock_try(void) {
    return mutex_trylock(&format_lock);
}

void format_lock_release(void) {
    mutex_unlock(&format_lock);
}

const char* format_result_text(FormatResult result) {
    switch (result) {
        case FORMAT_OK:             return "done";
        case FORMAT_NO_DEVICE:      return "no such device";
        case FORMAT_NOT_MOUNTED:    return "the partition is not mounted";
        case FORMAT_BAD_FILESYSTEM: return "unknown file system";
        case FORMAT_BAD_BLOCK_SIZE: return "the file system does not take that block size";
        case FORMAT_TOO_LARGE:      return "larger than the device";
        case FORMAT_TOO_SMALL:      return "too small for that file system";
        case FORMAT_SYSTEM_DEVICE:  return "holds the system folders";
        case FORMAT_BUSY:           return "device busy: close open files on it first";
        case FORMAT_IN_PROGRESS:    return "another format is running";
        case FORMAT_WIPE_FAILED:    return "wipe failed, the drive is in an unknown state";
        case FORMAT_IO_ERROR:       return "format failed";
    }
    return "format failed";
}
