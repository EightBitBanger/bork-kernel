#include <kernel/arch/x86/bus/usb/usb.h>
#include <kernel/arch/x86/bus/usb/usb_internal.h>
#include <kernel/arch/x86/bus/pci/pci.h>
#include <kernel/block/block.h>
#include <kernel/block/partition.h>
#include <kernel/vfs/vfs.h>

#include <kernel/malloc.h>
#include <kernel/util/string.h>

//
// USB mass storage: Bulk-Only Transport with SCSI commands
// (USB Mass Storage Class Bulk-Only Transport 1.0, SCSI SBC / SPC)
//
// Flash drives, card readers and USB hard disks. Each one becomes a block
// device named usb0, usb1, ... and is mounted in /mnt like any other disk
// (its partitions as usb0p1, ...). LUN 0 only.
//
// Plugging and unplugging happens on the "usb" thread with the controller
// locked, while mounting reads the disk through that same controller, so
// the two halves are split: attach and detach run under the lock and only
// mark the disk; usb_storage_service() later mounts new disks and takes
// gone ones out of /mnt and the block layer.
//
// Every command runs with the controller locked, from whichever thread
// reads or writes the disk.
//

#define USB_CLASS_STORAGE           0x08
#define USB_STORAGE_PROTOCOL_BOT    0x50

#define USB_STORAGE_REQ_RESET       0xFF    // Bulk-Only Mass Storage Reset
#define USB_FEATURE_ENDPOINT_HALT   0

#define USB_STORAGE_MAX_DISKS       8

#define CBW_SIGNATURE               0x43425355U     // "USBC"
#define CSW_SIGNATURE               0x53425355U     // "USBS"
#define CBW_FLAG_IN                 0x80

#define CSW_PASSED                  0
#define CSW_FAILED                  1
#define CSW_PHASE_ERROR             2

// SCSI
#define SCSI_TEST_UNIT_READY        0x00
#define SCSI_REQUEST_SENSE          0x03
#define SCSI_INQUIRY                0x12
#define SCSI_READ_CAPACITY_10       0x25
#define SCSI_READ_10                0x28
#define SCSI_WRITE_10               0x2A
#define SCSI_SYNCHRONIZE_CACHE_10   0x35

#define SCSI_TYPE_DIRECT_ACCESS     0x00

#define USB_STORAGE_CMD_TIMEOUT     2000
#define USB_STORAGE_DATA_TIMEOUT    10000
#define USB_STORAGE_READY_TRIES     20      // TEST UNIT READY, 100 ms apart
#define USB_STORAGE_IO_TRIES        3
#define USB_STORAGE_MAX_SECTORS     64      // Per READ / WRITE command

struct __attribute__((packed)) UsbCbw {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_length;
    uint8_t  flags;
    uint8_t  lun;
    uint8_t  cb_length;
    uint8_t  cb[16];
};

struct __attribute__((packed)) UsbCsw {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;
};

struct UsbDisk {
    struct UsbController* hc;   // Controllers are never freed
    struct UsbDevice*     dev;  // NULL once unplugged
    uint8_t  interface;
    struct UsbBulkPipe in;
    struct UsbBulkPipe out;
    uint32_t tag;

    uint32_t sector_size;
    uint64_t sectors;
    struct BlockDevice* block;

    bool gone;                  // Unplugged: I/O fails, service() removes it
    bool mount_pending;         // Registered, not yet mounted
};

// Changed by the "usb" thread only (and at boot, before it runs)
static struct UsbDisk* disks[USB_STORAGE_MAX_DISKS];
static uint32_t disk_count = 0;

static inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

//
// Bulk-Only Transport (hc->lock held)
//

static void usb_disk_clear_halt(struct UsbDisk* disk, struct UsbBulkPipe* pipe) {
    uint16_t endpoint = (uint16_t)(pipe->endpoint | (pipe->in ? 0x80 : 0));
    usb_control_locked(disk->dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT,
                       USB_REQ_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT, endpoint, NULL, 0);
    disk->hc->ops->bulk_reset(disk->hc, pipe);
}

// After a phase error or a broken CSW (BOT 5.3.4)
static void usb_disk_reset_recovery(struct UsbDisk* disk) {
    usb_control_locked(disk->dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                       USB_STORAGE_REQ_RESET, 0, disk->interface, NULL, 0);
    usb_disk_clear_halt(disk, &disk->in);
    usb_disk_clear_halt(disk, &disk->out);
}

static int usb_disk_bulk(struct UsbDisk* disk, struct UsbBulkPipe* pipe, void* data, uint32_t length, uint32_t timeout) {
    return disk->hc->ops->bulk_transfer(disk->hc, pipe, data, length, timeout);
}

// One SCSI command: CBW, data stage (IN into / OUT from `data`), CSW.
// Returns CSW_PASSED, CSW_FAILED (check condition: ask REQUEST SENSE), or
// a negative USB_ERR_* when the transport broke.
static int usb_disk_command(struct UsbDisk* disk, const uint8_t* cdb, uint8_t cdb_length,
                            bool in, void* data, uint32_t length) {
    if (disk->gone || disk->dev == NULL)
        return USB_ERR_IO;

    struct UsbCbw cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.signature   = CBW_SIGNATURE;
    cbw.tag         = ++disk->tag;
    cbw.data_length = length;
    cbw.flags       = in ? CBW_FLAG_IN : 0;
    cbw.lun         = 0;
    cbw.cb_length   = cdb_length;
    memcpy(cbw.cb, cdb, cdb_length);

    int result = usb_disk_bulk(disk, &disk->out, &cbw, sizeof(cbw), USB_STORAGE_CMD_TIMEOUT);
    if (result != (int)sizeof(cbw)) {
        usb_disk_reset_recovery(disk);
        return result < 0 ? result : USB_ERR_IO;
    }

    // Data stage, a bounce buffer's worth at a time. A short IN ends it
    // early; a stall ends it too, and the CSW still follows.
    struct UsbBulkPipe* pipe = in ? &disk->in : &disk->out;
    uint8_t* bytes = (uint8_t*)data;
    for (uint32_t done = 0; done < length; ) {
        uint32_t chunk = length - done < USB_BULK_MAX ? length - done : USB_BULK_MAX;
        result = usb_disk_bulk(disk, pipe, bytes + done, chunk, USB_STORAGE_DATA_TIMEOUT);
        if (result == USB_ERR_STALL) {
            usb_disk_clear_halt(disk, pipe);
            break;
        }
        if (result < 0) {
            usb_disk_reset_recovery(disk);
            return result;
        }
        done += (uint32_t)result;
        if ((uint32_t)result < chunk)
            break;
    }

    // Status stage: one retry after a stall (BOT 6.7.2)
    struct UsbCsw csw;
    result = usb_disk_bulk(disk, &disk->in, &csw, sizeof(csw), USB_STORAGE_DATA_TIMEOUT);
    if (result == USB_ERR_STALL) {
        usb_disk_clear_halt(disk, &disk->in);
        result = usb_disk_bulk(disk, &disk->in, &csw, sizeof(csw), USB_STORAGE_DATA_TIMEOUT);
    }
    if (result != (int)sizeof(csw) || csw.signature != CSW_SIGNATURE || csw.tag != cbw.tag ||
        csw.status == CSW_PHASE_ERROR) {
        usb_disk_reset_recovery(disk);
        return result < 0 ? result : USB_ERR_IO;
    }
    return csw.status == CSW_PASSED ? CSW_PASSED : CSW_FAILED;
}

// Read and drop the sense data, which also clears the check condition
static void usb_disk_request_sense(struct UsbDisk* disk) {
    uint8_t cdb[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
    uint8_t sense[18];
    usb_disk_command(disk, cdb, sizeof(cdb), true, sense, sizeof(sense));
}

// A command retried after check conditions and transport errors
static bool usb_disk_command_retry(struct UsbDisk* disk, const uint8_t* cdb, uint8_t cdb_length,
                                   bool in, void* data, uint32_t length) {
    for (int attempt = 0; attempt < USB_STORAGE_IO_TRIES && !disk->gone; attempt++) {
        int result = usb_disk_command(disk, cdb, cdb_length, in, data, length);
        if (result == CSW_PASSED)
            return true;
        if (result == CSW_FAILED)
            usb_disk_request_sense(disk);
    }
    return false;
}

//
// Block device
//

static bool usb_disk_transfer(struct UsbDisk* disk, uint8_t opcode, uint64_t lba, uint32_t count, void* buffer) {
    if (lba + count > 0xFFFFFFFFULL)
        return false;   // READ(10) / WRITE(10) reach 2 TB of 512-byte sectors

    bool ok = true;
    mutex_lock(&disk->hc->lock);
    uint8_t* bytes = (uint8_t*)buffer;
    while (ok && count > 0) {
        uint32_t n = count < USB_STORAGE_MAX_SECTORS ? count : USB_STORAGE_MAX_SECTORS;
        uint32_t block = (uint32_t)lba;
        uint8_t cdb[10] = {
            opcode, 0,
            (uint8_t)(block >> 24), (uint8_t)(block >> 16), (uint8_t)(block >> 8), (uint8_t)block,
            0, (uint8_t)(n >> 8), (uint8_t)n, 0,
        };
        ok = usb_disk_command_retry(disk, cdb, sizeof(cdb), opcode == SCSI_READ_10, bytes, n * disk->sector_size);
        bytes += n * disk->sector_size;
        lba   += n;
        count -= n;
    }
    mutex_unlock(&disk->hc->lock);
    return ok;
}

static bool usb_disk_read(struct BlockDevice* dev, uint64_t lba, uint32_t count, void* buffer) {
    return usb_disk_transfer((struct UsbDisk*)dev->driver_data, SCSI_READ_10, lba, count, buffer);
}

static bool usb_disk_write(struct BlockDevice* dev, uint64_t lba, uint32_t count, const void* buffer) {
    return usb_disk_transfer((struct UsbDisk*)dev->driver_data, SCSI_WRITE_10, lba, count, (void*)buffer);
}

static bool usb_disk_flush(struct BlockDevice* dev) {
    struct UsbDisk* disk = (struct UsbDisk*)dev->driver_data;
    mutex_lock(&disk->hc->lock);
    bool gone = disk->gone;
    if (!gone) {
        // Many flash drives have no cache to flush and fail this: fine
        uint8_t cdb[10] = { SCSI_SYNCHRONIZE_CACHE_10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        if (usb_disk_command(disk, cdb, sizeof(cdb), false, NULL, 0) == CSW_FAILED)
            usb_disk_request_sense(disk);
    }
    mutex_unlock(&disk->hc->lock);
    return !gone;
}

static const struct BlockDeviceOps usb_disk_ops = {
    .read  = usb_disk_read,
    .write = usb_disk_write,
    .flush = usb_disk_flush,
};

//
// Attach / detach (hc->lock held)
//

static void usb_disk_free(struct UsbDisk* disk) {
    if (disk->dev != NULL) {
        disk->hc->ops->bulk_close(disk->hc, &disk->in);
        disk->hc->ops->bulk_close(disk->hc, &disk->out);
    }
    free(disk);
}

// Wake it up and size it. False if it is not a disk or never got ready.
static bool usb_disk_probe(struct UsbDisk* disk) {
    uint8_t inquiry[36];
    uint8_t cdb_inquiry[6] = { SCSI_INQUIRY, 0, 0, 0, sizeof(inquiry), 0 };
    memset(inquiry, 0, sizeof(inquiry));
    if (!usb_disk_command_retry(disk, cdb_inquiry, sizeof(cdb_inquiry), true, inquiry, sizeof(inquiry)))
        return false;
    if ((inquiry[0] & 0x1F) != SCSI_TYPE_DIRECT_ACCESS)
        return false;   // CD-ROM and the like

    // Card readers without a card, and drives spinning up, report not ready
    uint8_t cdb_ready[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    bool ready = false;
    for (int attempt = 0; attempt < USB_STORAGE_READY_TRIES && !ready; attempt++) {
        int result = usb_disk_command(disk, cdb_ready, sizeof(cdb_ready), false, NULL, 0);
        ready = result == CSW_PASSED;
        if (!ready) {
            if (result == CSW_FAILED)
                usb_disk_request_sense(disk);
            usb_delay_ms(100);
        }
    }
    if (!ready)
        return false;

    uint8_t capacity[8];
    uint8_t cdb_capacity[10] = { SCSI_READ_CAPACITY_10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (!usb_disk_command_retry(disk, cdb_capacity, sizeof(cdb_capacity), true, capacity, sizeof(capacity)))
        return false;

    uint32_t last_lba = be32(&capacity[0]);
    disk->sector_size = be32(&capacity[4]);
    disk->sectors = (uint64_t)last_lba + 1;
    if (last_lba == 0xFFFFFFFFU)
        disk->sectors = 0xFFFFFFFFULL;  // Over 2 TB: READ(10) reaches this much
    return disk->sector_size >= 512 && disk->sector_size <= 4096 && last_lba != 0;
}

void usb_storage_attach(struct UsbDevice* dev) {
    struct UsbController* hc = dev->hc;
    if (hc->ops->bulk_open == NULL || disk_count >= USB_STORAGE_MAX_DISKS)
        return;

    const struct UsbInterface* itf = NULL;
    for (uint32_t i = 0; i < dev->interfaces_known && itf == NULL; i++) {
        const struct UsbInterface* candidate = &dev->interfaces[i];
        if (candidate->if_class == USB_CLASS_STORAGE && candidate->if_protocol == USB_STORAGE_PROTOCOL_BOT &&
            candidate->bulk_in_endpoint != 0 && candidate->bulk_out_endpoint != 0)
            itf = candidate;
    }
    if (itf == NULL)
        return;

    struct UsbDisk* disk = (struct UsbDisk*)malloc(sizeof(struct UsbDisk));
    if (disk == NULL)
        return;
    memset(disk, 0, sizeof(*disk));
    disk->hc        = hc;
    disk->dev       = dev;
    disk->interface = itf->number;

    disk->in.dev         = dev;
    disk->in.endpoint    = itf->bulk_in_endpoint;
    disk->in.in          = true;
    disk->in.max_packet  = itf->bulk_in_max_packet;
    disk->out.dev        = dev;
    disk->out.endpoint   = itf->bulk_out_endpoint;
    disk->out.in         = false;
    disk->out.max_packet = itf->bulk_out_max_packet;

    if (!hc->ops->bulk_open(hc, &disk->in)) {
        free(disk);
        return;
    }
    if (!hc->ops->bulk_open(hc, &disk->out)) {
        hc->ops->bulk_close(hc, &disk->in);
        free(disk);
        return;
    }

    if (!usb_disk_probe(disk)) {
        usb_print("USB: storage device not ready, not mounted\n");
        usb_disk_free(disk);
        return;
    }

    disk->block = block_register("usb", &usb_disk_ops, disk, disk->sector_size, disk->sectors);
    if (disk->block == NULL) {
        usb_print("USB: no room for another block device\n");
        usb_disk_free(disk);
        return;
    }

    disk->mount_pending = true;
    disks[disk_count++] = disk;
}

void usb_storage_detach(struct UsbDevice* dev) {
    for (uint32_t i = 0; i < disk_count; i++) {
        struct UsbDisk* disk = disks[i];
        if (disk->dev != dev)
            continue;

        // The device is gone: drop the pipes now, and let I/O fail until
        // service() takes the disk out
        dev->hc->ops->bulk_close(dev->hc, &disk->in);
        dev->hc->ops->bulk_close(dev->hc, &disk->out);
        disk->gone = true;
        disk->dev  = NULL;
    }
}

//
// Mount and unmount (no controller lock held)
//

void usb_storage_service(void) {
    for (uint32_t i = 0; i < disk_count; ) {
        struct UsbDisk* disk = disks[i];

        if (disk->gone) {
            // Mounts first (they point at the partitions), then the
            // partitions, then the disk itself
            vfs_device_remove_disk(disk->block);
            block_partition_remove_all(disk->block);
            block_unregister(disk->block);

            // A thread already past the gone check holds the lock until
            // its command fails; wait it out before freeing
            mutex_lock(&disk->hc->lock);
            mutex_unlock(&disk->hc->lock);
            free(disk);

            for (uint32_t j = i; j + 1 < disk_count; j++)
                disks[j] = disks[j + 1];
            disk_count--;
            continue;
        }

        if (disk->mount_pending) {
            disk->mount_pending = false;
            pci_mount_removable(disk->block, usb_is_quiet());
        }
        i++;
    }
}
