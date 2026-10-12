#include <kernel/arch/x86/io.h>
#include <kernel/arch/x86/heap.h>
#include <kernel/arch/x86/bus/pci/pci.h>
#include <kernel/arch/x86/bus/pci/pci_internal.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/arch/x86/drivers/ahci/ahci.h>
#include <kernel/arch/x86/drivers/ata/ata.h>
#include <kernel/block/block.h>
#include <kernel/block/partition.h>

#include <kernel/kernel.h>
#include <kernel/knode.h>
#include <kernel/fs/fs.h>
#include <kernel/vfs/vfs.h>

#include <kernel/console/print.h>
#include <kernel/util/string.h>

#define PCI_COMMAND_IO          (1U << 0)
#define PCI_COMMAND_MEMORY      (1U << 1)
#define PCI_COMMAND_BUS_MASTER  (1U << 2)


const char* pci_get_class_name(uint8_t class_code) {
    switch (class_code) {
        case 0x00: return "legacy";
        case 0x01: return "storage";
        case 0x02: return "network";
        case 0x03: return "display";
        case 0x04: return "multimedia";
        case 0x06: return "bridge";
        case 0x0C: return "serialusb";
        default:   return "unknown";
    }
}

// Set once a mounted device with a /sys directory has claimed the home path.
// Before this, the last device mounted took home unconditionally, so adding a
// blank disk could silently move home (and the registry) onto it.
static bool storage_home_has_sys = false;

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
    
    // A device with /sys always takes home (last one wins, as before).
    // A device without it only takes home while no /sys device has.
    if (has_sys || !storage_home_has_sys) {
        strncpy(fs_paths->home, mount_root, PATH_LENGTH_MAX);
        if (has_sys) storage_home_has_sys = true;
    }
}

// Mount a block device as /mnt/<name>. Mount knode references:
//   0: the device's one-sector cache window (owned by this mount)
//   1: its FSDeviceContext, which carries the BlockDevice*
// The filesystem never learns what kind of disk this is.
static void pci_mount_block_device(struct BlockDevice* device, uint32_t mnt_dir, struct LocalPaths* fs_paths) {
    if (device == NULL) return;
    
    // Mounted before and detached since (the disk was rewritten by "boot
    // install"): open the device again in the same knode, window and
    // context instead of adding a second /mnt entry with the same name
    uint32_t existing = knode_find_by_name(mnt_dir, device->name);
    if (existing != 0 && existing != KNODE_NULL && existing != KMALLOC_NULL &&
        (kmalloc_get_flags(existing) & KMALLOC_FLAG_MOUNT) != 0) {
        uint32_t old_window = knode_get_reference(existing, 0);
        uint32_t old_ctx    = knode_get_reference(existing, 1);
        if (old_window != 0 && old_window != KMALLOC_NULL && old_ctx != 0 && old_ctx != KMALLOC_NULL) {
            struct FSPartitionBlock part;
            vfs_lock();
            struct FSDeviceContext* ctx = (struct FSDeviceContext*)old_ctx;
            if (ctx->device == NULL || ctx->device == device)
                *ctx = fs_device_open(device, old_window, &part);
            bool open = ctx->is_open;
            vfs_unlock();
            
            if (open)
                pci_setup_storage_paths(device->name, fs_paths);
            print(open ? "Remounted " : "Remounted (unformatted) ");
            print(device->name);
            print("\n");
            return;
        }
    }
    
    uint32_t window_size = fs_device_window_size(device);
    uint32_t window = (uint32_t)malloc(window_size);
    if (window == 0) {
        print("Storage Error: out of memory mounting ");
        print(device->name);
        print("\n");
        return;
    }
    memset((void*)window, 0, window_size);
    
    struct FSPartitionBlock part;
    struct FSDeviceContext context = fs_device_open(device, window, &part);
    
    uint32_t device_context = (uint32_t)malloc(sizeof(struct FSDeviceContext));
    if (device_context == 0) {
        free((void*)window);
        return;
    }
    memcpy((void*)device_context, &context, sizeof(struct FSDeviceContext));
    
    uint32_t mount_ptr = create_knode(device->name, mnt_dir);
    kmalloc_set_flags(mount_ptr, (KMALLOC_FLAG_DIRECTORY | KMALLOC_FLAG_MOUNT));
    
    knode_add_reference(mount_ptr, window);
    knode_add_reference(mount_ptr, device_context);
    
    // An unformatted disk still mounts (so it can be formatted), but has no
    // bin or sys folders to offer
    if (context.is_open)
        pci_setup_storage_paths(device->name, fs_paths);
    
    print(context.is_open ? "Mounted " : "Mounted (unformatted) ");
    print(device->name);
    print("\n");
}

struct PciMountArgs {
    uint32_t mnt_dir;
    struct LocalPaths* fs_paths;
};

static void pci_mount_callback(struct BlockDevice* device, void* arg) {
    struct PciMountArgs* args = (struct PciMountArgs*)arg;
    pci_mount_block_device(device, args->mnt_dir, args->fs_paths);
}

static bool pci_probe_basefs(const uint8_t* sector0, uint32_t sector_size) {
    return fs_device_probe(sector0, sector_size);
}

// Mount what a newly registered disk holds: a whole-disk basefs volume, the
// basefs partitions of its MBR (a boot disk), or the blank disk itself so
// it can be formatted. A disk with a partition table is never mounted
// whole, so nothing in the VFS can format over the table, the boot loader
// or the kernel image in front of the first partition.
static void pci_mount_disk(struct BlockDevice* disk, uint32_t mnt_dir, struct LocalPaths* fs_paths) {
    if (disk == NULL) return;
    
    struct PciMountArgs args = { mnt_dir, fs_paths };
    struct BlockScanReport report = block_scan_disk(disk, MBR_TYPE_BASEFS, pci_probe_basefs,
                                                    pci_mount_callback, &args);
    
    switch (report.result) {
        case BLOCK_SCAN_NO_PARTITIONS:
            print(disk->name);
            print(": partition table has no basefs partitions, not mounted\n");
            break;
        case BLOCK_SCAN_ERROR:
            print(disk->name);
            print(": could not register its partitions\n");
            break;
        default:
            break;
    }
    if (report.skipped > 0) {
        print(disk->name);
        print(": skipped a basefs partition that does not fit the disk\n");
    }
}

// Probe both channels of an IDE controller, master and slave on each, and
// mount every ATA disk found. ATAPI drives (the boot CD-ROM) are skipped.
//
// prog_if bit 0 / bit 2: primary / secondary channel in PCI native mode
// (I/O ports from BAR0-1 / BAR2-3); otherwise the legacy ISA ports.
static void pci_handle_ata(uint8_t bus, uint8_t dev, uint8_t func, uint8_t prog_if, uint32_t mnt_dir, struct LocalPaths* fs_paths) {
    uint16_t io_base[2], ctrl_base[2];
    
    if (prog_if & 0x01) {
        io_base[0]   = pci_get_io_bar(bus, dev, func, 0);
        uint16_t ctl = pci_get_io_bar(bus, dev, func, 1);
        ctrl_base[0] = ctl ? (uint16_t)(ctl + 2) : 0;   // Device control is at BAR1 + 2
    } else {
        io_base[0]   = ATA_PRIMARY_IO;
        ctrl_base[0] = ATA_PRIMARY_CTRL;
    }
    
    if (prog_if & 0x04) {
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
            print("ATA Error: could not register disk\n");
            continue;
        }
        pci_mount_disk(device, mnt_dir, fs_paths);
    }
}

static void pci_handle_ahci(uint8_t bus, uint8_t dev, uint8_t func, uint32_t mnt_dir, struct LocalPaths* fs_paths) {
    // Bus mastering + memory decode (already set by pci_handle_storage_device,
    // repeated here so this handler stands on its own)
    pci_enable_command_bits(bus, dev, func, PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER);
    
    // ABAR is BAR5. Map only the HBA register block, once.
    uint32_t flags = VM_PRESENT | VM_READWRITE | VM_PCD; 
    struct AHCI_HBA_Memory_Space* hba = 
        (struct AHCI_HBA_Memory_Space*)pci_map_bar(bus, dev, func, 5, AHCI_HBA_MMIO_SIZE, flags); 
    if (hba == NULL) return;
    
    ahci_init(hba); 
    
    // Mount every port of THIS controller that ahci_init() brought up.
    // Previously only the first SATA port was mounted and the rest ignored.
    for (int p = 0; p < 32; p++) {
        if (!(hba->ports_implemented & (1U << p)))
            continue;
        
        struct AHCI_Port_Registers* port = &hba->ports[p];
        if (!ahci_port_ready(port) || block_find_driver(port) != NULL)
            continue;   // Not usable, or already mounted
        
        struct BlockDevice* device = ahci_create_block_device(port);
        if (device != NULL)
            pci_mount_disk(device, mnt_dir, fs_paths);
    }
}

void pci_handle_storage_device(uint8_t bus, uint8_t dev, uint8_t func, uint8_t subclass, uint8_t prog_if, uint32_t mnt_dir, struct LocalPaths* fs_paths) {
    // Writes only the command half of the register, so pending (RW1C)
    // status bits are no longer cleared as a side effect
    pci_enable_command_bits(bus, dev, func, PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER);
    
    if (subclass == 0x01) {
        // IDE uses I/O port BARs (or the legacy 0x1F0 ports): enable I/O decode
        pci_enable_command_bits(bus, dev, func, PCI_COMMAND_IO);
        pci_handle_ata(bus, dev, func, prog_if, mnt_dir, fs_paths);
    } else if (subclass == 0x06) {
        pci_handle_ahci(bus, dev, func, mnt_dir, fs_paths);
    }
}

void pci_init(void) {
    uint32_t root_node = knode_get_root();
    uint32_t dev_directory = knode_find_by_name(root_node, "dev");
    uint32_t mnt_directory = knode_find_by_name(root_node, "mnt");
    uint32_t pci_directory = create_knode("pci", dev_directory);
    
    uint32_t bus0_directory = create_knode("bus0", pci_directory);
    
    knode_set_permissions(pci_directory, KMALLOC_PERMISSION_READ);
    knode_set_permissions(bus0_directory, KMALLOC_PERMISSION_READ);
    
    // Read the paths once, let every storage device on every bus update the
    // same copy, then publish it once. Previously each (recursive) bus scan
    // took its own copy and wrote it back on exit, so a parent bus finishing
    // after a bridge overwrote whatever the child bus had found.
    struct LocalPaths fs_paths;
    kernel_get_local_paths(&fs_paths);
    
    pci_scan_reset();
    pci_scan_bus(0, bus0_directory, mnt_directory, &fs_paths, 0);
    
    kernel_set_local_paths(&fs_paths);
}

// Mount whatever `disk` holds now, from a running kernel (after "boot
// install" rewrote it). Same rules as the boot-time scan.
void pci_mount_storage(struct BlockDevice* disk) {
    uint32_t root = knode_get_root();
    uint32_t mnt_directory = knode_find_by_name(root, "mnt");
    if (disk == NULL || mnt_directory == 0 || mnt_directory == KNODE_NULL || mnt_directory == KMALLOC_NULL)
        return;
    
    struct LocalPaths fs_paths;
    kernel_get_local_paths(&fs_paths);
    pci_mount_disk(disk, mnt_directory, &fs_paths);
    kernel_set_local_paths(&fs_paths);
}
