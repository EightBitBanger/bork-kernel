#include <kernel/arch/x86/heap.h>
#include <kernel/arch/x86/bus/pci/pci.h>
#include <kernel/arch/x86/bus/pci/pci_internal.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/arch/x86/drivers/ahci/ahci.h>
#include <kernel/arch/x86/drivers/ata/ata.h>
#include <kernel/block/block.h>
#include <kernel/block/partition.h>
#include <kernel/boot/x86/boot_image.h>

#include <kernel/kernel.h>
#include <kernel/knode.h>
#include <kernel/fs/fs_type.h>
#include <kernel/vfs/vfs.h>

#include <kernel/console/print.h>
#include <kernel/util/string.h>

// How good a claim the device holding the home path has, best first:
//   3: a device with /sys on the disk the loader booted from
//   2: any other device with /sys
//   1: a device without /sys
// A better claim always takes home. Within ranks 1 and 2 the last device
// mounted wins, as before; within rank 3 the first one keeps it. Without
// this, with several installed disks, home (and the registry) could land
// on a disk other than the one the machine booted from.
static uint8_t storage_home_rank = 0;

// Set by pci_mount_disk while it mounts the volumes of the boot disk
static bool storage_mounting_boot_disk = false;

// First disk found with the boot disk's signature
static struct BlockDevice* storage_boot_disk = NULL;

// Whether that disk was mounted as a removable one (USB)
static bool storage_boot_disk_removable = false;

// Set while mounting a disk plugged in at run time, when the desktop may
// own the screen: nothing is printed then
static bool storage_quiet = false;

// Set while mounting a removable disk (USB): one whose partitions are all of
// filesystems the VFS doesn't know (an NTFS or exFAT stick) is mounted
// whole, as unformatted, so it still shows in /mnt and can be formatted. A
// fixed disk like that (another OS) stays out of /mnt.
static bool storage_removable = false;

static void storage_print(const char* text) {
    if (!storage_quiet) print(text);
}

// Sector 0 of a disk being checked. Static, so DMA drivers can reach it.
static uint8_t storage_sector0[4096] __attribute__((aligned(16)));

static void pci_setup_storage_paths(const char* device_name, struct LocalPaths* fs_paths) {
    char mount_root[64];
    memset(mount_root, '\0', sizeof(mount_root));
    strcat(mount_root, "/mnt/");
    strncat(mount_root, device_name, sizeof(mount_root) - strlen(mount_root) - 1);

    char bin_path[64];
    memset(bin_path, '\0', sizeof(bin_path));
    strncpy(bin_path, mount_root, sizeof(bin_path) - 1);
    strcat(bin_path, "/bin");

    if (vfs_directory_check(bin_path)) {
        strcat(bin_path, ";");
        strncpy(fs_paths->path, bin_path, PATH_LENGTH_MAX);
    }

    if (!vfs_directory_check(mount_root))
        return;

    char sys_path[64];
    memset(sys_path, '\0', sizeof(sys_path));
    strncpy(sys_path, mount_root, sizeof(sys_path) - 1);
    strcat(sys_path, "/sys");
    bool has_sys = vfs_directory_check(sys_path);

    uint8_t rank = !has_sys ? 1 : (storage_mounting_boot_disk ? 3 : 2);
    if (rank > storage_home_rank || (rank == storage_home_rank && rank < 3)) {
        strncpy(fs_paths->home, mount_root, PATH_LENGTH_MAX);
        storage_home_rank = rank;
    }
}

// Mount a block device as /mnt/<name>, with whichever filesystem the VFS
// finds on it (or unformatted, so it can be formatted). The VFS never
// learns what kind of disk this is.
static void pci_mount_block_device(struct BlockDevice* device, void* mnt_dir, struct LocalPaths* fs_paths) {
    if (device == NULL) return;

    // A device mounted before and detached since (the disk was rewritten by
    // "boot install") is re-attached in its existing knode
    bool remounted = false;
    struct FsMount* mount = vfs_mount_device(device, mnt_dir, &remounted);
    if (mount == NULL) {
        storage_print("Storage Error: out of memory mounting ");
        storage_print(device->name);
        storage_print("\n");
        return;
    }

    vfs_lock();
    bool open = mount->is_open;
    // Home and the program path only on a filesystem that keeps security
    // masks: on one that cannot (FAT) the system files would be open to
    // every program, and any stick with a bin folder could take over the path
    bool system_capable = mount->type->set_security != NULL;
    vfs_unlock();

    // An unformatted disk still mounts (so it can be formatted), but has no
    // bin or sys folders to offer
    if (open && system_capable)
        pci_setup_storage_paths(device->name, fs_paths);

    if (remounted)
        storage_print(open ? "Remounted " : "Remounted (unformatted) ");
    else
        storage_print(open ? "Mounted " : "Mounted (unformatted) ");
    storage_print(device->name);
    storage_print("\n");
}

struct PciMountArgs {
    void* mnt_dir;
    struct LocalPaths* fs_paths;
};

static void pci_mount_callback(struct BlockDevice* device, void* arg) {
    struct PciMountArgs* args = (struct PciMountArgs*)arg;
    pci_mount_block_device(device, args->mnt_dir, args->fs_paths);
}

static bool pci_probe_volume(const uint8_t* sector0, uint32_t sector_size) {
    return vfs_fs_probe(sector0, sector_size);
}

// Disk signature of the boot disk, from sector 0 of the boot area stage 2
// read off the boot drive. 0 when there is no boot area (another loader)
// or the boot disk has no signature.
static uint32_t pci_boot_disk_signature(void) {
    uint32_t size = 0;
    const uint8_t* image = boot_image_get(&size);
    if (image == NULL || size < MBR_SECTOR_SIZE)
        return 0;
    return block_mbr_signature(image);
}

// True when `disk` is the disk the loader booted from: its MBR carries the
// boot disk's non-zero signature. The BIOS drive number in the Multiboot
// boot_device field cannot say which ATA or AHCI disk that is; the
// signature can. Whole-disk volumes and partitions never match.
static bool pci_is_boot_disk(struct BlockDevice* disk) {
    uint32_t boot_signature = pci_boot_disk_signature();
    if (boot_signature == 0 || block_partition_parent(disk) != NULL)
        return false;
    if (disk->sector_size < MBR_SECTOR_SIZE || disk->sector_size > sizeof(storage_sector0))
        return false;
    if (!block_read(disk, 0, 1, storage_sector0))
        return false;
    if (pci_probe_volume(storage_sector0, disk->sector_size))
        return false;
    return block_mbr_signature(storage_sector0) == boot_signature;
}

// Mount what a newly registered disk holds: a whole-disk volume, the
// partitions of its MBR that belong to a known filesystem (a boot disk), or
// the blank disk itself so it can be formatted. A disk with a partition table is never mounted
// whole, so nothing in the VFS can format over the table, the boot loader
// or the kernel image in front of the first partition.
static void pci_mount_disk(struct BlockDevice* disk, void* mnt_dir, struct LocalPaths* fs_paths) {
    if (disk == NULL) return;

    // A second disk with the same signature is a byte copy of the boot disk
    // (dd). Nothing tells the two apart, so the first one found keeps home.
    bool is_boot_disk = pci_is_boot_disk(disk);
    if (is_boot_disk) {
        if (storage_boot_disk == NULL) {
            storage_boot_disk = disk;
            storage_boot_disk_removable = storage_removable;
        } else if (storage_boot_disk != disk) {
            storage_print(disk->name);
            storage_print(": same disk signature as ");
            storage_print(storage_boot_disk->name);
            storage_print(", not used as the boot disk\n");
            is_boot_disk = false;
        }
    }

    struct PciMountArgs args = { mnt_dir, fs_paths };
    storage_mounting_boot_disk = is_boot_disk;
    struct BlockScanReport report = block_scan_disk(disk, vfs_fs_claims_partition, pci_probe_volume,
                                                    pci_mount_callback, &args);
    storage_mounting_boot_disk = false;

    switch (report.result) {
        case BLOCK_SCAN_NO_PARTITIONS:
            if (storage_removable) {
                pci_mount_block_device(disk, mnt_dir, fs_paths);
                break;
            }
            storage_print(disk->name);
            storage_print(": partition table has no partitions of a known filesystem, not mounted\n");
            break;
        case BLOCK_SCAN_ERROR:
            storage_print(disk->name);
            storage_print(": could not register its partitions\n");
            break;
        default:
            break;
    }
    if (report.skipped > 0) {
        storage_print(disk->name);
        storage_print(": skipped a partition that does not fit the disk\n");
    }
}

// Probe both channels of an IDE controller, master and slave on each, and
// mount every ATA disk found. ATAPI drives (the boot CD-ROM) are skipped.
//
// prog_if bit 0 / bit 2: primary / secondary channel in PCI native mode
// (I/O ports from BAR0-1 / BAR2-3); otherwise the legacy ISA ports.
void pci_storage_probe_ide(const struct PCIDeviceInfo* info, struct PCIScanContext* ctx) {
    uint8_t bus = info->bus, dev = info->dev, func = info->func;

    // Writes only the command half of the register, so pending (RW1C)
    // status bits are not cleared as a side effect. IDE uses I/O port BARs
    // (or the legacy 0x1F0 ports), so I/O decode is needed as well.
    pci_enable_command_bits(bus, dev, func, PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER | PCI_COMMAND_IO);

    uint16_t io_base[2], ctrl_base[2];

    if (info->prog_if & 0x01) {
        io_base[0]   = pci_get_io_bar(bus, dev, func, 0);
        uint16_t ctl = pci_get_io_bar(bus, dev, func, 1);
        ctrl_base[0] = ctl ? (uint16_t)(ctl + 2) : 0;   // Device control is at BAR1 + 2
    } else {
        io_base[0]   = ATA_PRIMARY_IO;
        ctrl_base[0] = ATA_PRIMARY_CTRL;
    }

    if (info->prog_if & 0x04) {
        io_base[1]   = pci_get_io_bar(bus, dev, func, 2);
        uint16_t ctl = pci_get_io_bar(bus, dev, func, 3);
        ctrl_base[1] = ctl ? (uint16_t)(ctl + 2) : 0;
    } else {
        io_base[1]   = ATA_SECONDARY_IO;
        ctrl_base[1] = ATA_SECONDARY_CTRL;
    }

    uint8_t first_new = ata_device_count();
    for (int ch = 0; ch < 2; ch++) {
        if (io_base[ch] != 0)
            ata_probe_channel(io_base[ch], ctrl_base[ch]);
    }

    uint8_t total = ata_device_count();
    for (uint8_t i = first_new; i < total; i++) {
        struct BlockDevice* device = ata_create_block_device(ata_get_device(i));
        if (device == NULL) {
            storage_print("ATA Error: could not register disk\n");
            continue;
        }
        pci_mount_disk(device, ctx->mnt_dir, ctx->fs_paths);
    }
}

void pci_storage_probe_ahci(const struct PCIDeviceInfo* info, struct PCIScanContext* ctx) {
    uint8_t bus = info->bus, dev = info->dev, func = info->func;

    pci_enable_command_bits(bus, dev, func, PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER);

    // ABAR is BAR5. Map only the HBA register block, once.
    uint32_t flags = VM_PRESENT | VM_READWRITE | VM_PCD;
    struct AHCI_HBA_Memory_Space* hba =
        (struct AHCI_HBA_Memory_Space*)pci_map_bar(bus, dev, func, 5, AHCI_HBA_MMIO_SIZE, flags);
    if (hba == NULL) return;

    ahci_init(hba);

    // Mount every port of THIS controller that ahci_init() brought up
    for (int p = 0; p < 32; p++) {
        if (!(hba->ports_implemented & (1U << p)))
            continue;

        struct AHCI_Port_Registers* port = &hba->ports[p];
        if (!ahci_port_ready(port) || block_find_driver(port) != NULL)
            continue;   // Not usable, or already mounted

        struct BlockDevice* device = ahci_create_block_device(port);
        if (device != NULL)
            pci_mount_disk(device, ctx->mnt_dir, ctx->fs_paths);
    }
}

// Mount whatever `disk` holds now, from a running kernel (after fdisk,
// format or "boot install" rewrote it). Same rules as the boot-time scan;
// mounts of the disk the scan did not bring back are removed from /mnt.
void pci_mount_storage(struct BlockDevice* disk) {
    void* mnt_directory = knode_find_by_name(knode_get_root(), "mnt");
    if (disk == NULL || mnt_directory == NULL)
        return;

    struct LocalPaths fs_paths;
    kernel_get_local_paths(&fs_paths);
    pci_mount_disk(disk, mnt_directory, &fs_paths);
    vfs_device_prune_disk(disk);
    kernel_set_local_paths(&fs_paths);
}

void pci_mount_storage_quiet(struct BlockDevice* disk) {
    storage_quiet = true;
    pci_mount_storage(disk);
    storage_quiet = false;
}

void pci_mount_removable(struct BlockDevice* disk, bool quiet) {
    storage_quiet = quiet;
    storage_removable = true;
    pci_mount_storage(disk);
    storage_removable = false;
    storage_quiet = false;
}

// Rank 3 is only ever given to a device of the boot disk, so it says home's
// /sys is on that disk. Hotplugged disks never reach rank 3 ahead of it.
enum PciBootMedia pci_boot_media(void) {
    if (pci_boot_disk_signature() == 0)
        return PCI_BOOT_UNKNOWN;
    if (storage_boot_disk == NULL || storage_boot_disk_removable || storage_home_rank != 3)
        return PCI_BOOT_MEDIA;
    return PCI_BOOT_SYSTEM_DISK;
}
