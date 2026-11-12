
#include <kernel/arch/x86/drivers/ata/ata.h>
#include <kernel/arch/x86/io.h>
#include <kernel/util/string.h>
#include <kernel/util/timer.h>
#include <kernel/mutex.h>
#include <kernel/block/block.h>

// Register offsets from the I/O base
#define ATA_REG_DATA        0
#define ATA_REG_ERROR       1
#define ATA_REG_COUNT       2
#define ATA_REG_LBA_LO      3
#define ATA_REG_LBA_MID     4
#define ATA_REG_LBA_HI      5
#define ATA_REG_DRIVE       6
#define ATA_REG_STATUS      7       // Read
#define ATA_REG_COMMAND     7       // Write

#define ATA_SR_ERR          0x01
#define ATA_SR_DRQ          0x08
#define ATA_SR_DF           0x20
#define ATA_SR_BSY          0x80

#define ATA_CMD_READ        0x20
#define ATA_CMD_WRITE       0x30
#define ATA_CMD_FLUSH       0xE7
#define ATA_CMD_IDENTIFY    0xEC

#define ATA_CTRL_NIEN       0x02    // Device control: disable INTRQ (we poll)

// Drive/head register: bits 7 and 5 are obsolete-but-set, bit 6 = LBA,
// bit 4 = drive select (0 master, 1 slave), bits 0-3 = LBA 24-27
#define ATA_DRIVE_LBA(slave)    (0xE0 | ((slave) ? 0x10 : 0x00))
#define ATA_DRIVE_CHS(slave)    (0xA0 | ((slave) ? 0x10 : 0x00))

// Time-based timeouts (iteration counts vary wildly between machines)
#define ATA_TIMEOUT_MS          2000
#define ATA_FLUSH_TIMEOUT_MS    30000
#define ATA_IDENTIFY_TIMEOUT_MS 1000

#define ATA_MAX_CHANNELS    2

// Master and slave share one set of task file registers, so every command
// on a channel must run start to finish without the other drive's traffic
// interleaving. One mutex per channel serializes that.
struct AtaChannel {
    uint16_t io_base;
    uint16_t ctrl_base;
    bool     in_use;
    mutex_t  lock;
};

static struct AtaChannel ata_channels[ATA_MAX_CHANNELS];
static struct AtaDevice  ata_devices[ATA_MAX_DEVICES];
static uint8_t           ata_device_total = 0;

// Reading the status register four times is the standard ~400 ns delay the
// drive needs before its status is valid after a select or a command
static inline void ata_delay_400ns(uint16_t io_base) {
    for (int i = 0; i < 4; i++) inb(io_base + ATA_REG_STATUS);
}

// Wait for BSY to clear, then require (status & mask) == value.
// Fails early when the drive reports an error or a device fault.
static bool ata_wait(uint16_t io_base, uint8_t mask, uint8_t value, uint32_t timeout_ms) {
    uint64_t start = timer_get_ms();
    while (1) {
        uint8_t status = inb(io_base + ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY)) {
            if (status & (ATA_SR_ERR | ATA_SR_DF)) return false;
            if ((status & mask) == value) return true;
        }
        if (timer_get_ms() - start >= timeout_ms) return false;
    }
}

//
// Detection
//

static struct AtaChannel* ata_channel_get(uint16_t io_base, uint16_t ctrl_base) {
    for (int i = 0; i < ATA_MAX_CHANNELS; i++) {
        if (ata_channels[i].in_use && ata_channels[i].io_base == io_base)
            return &ata_channels[i];
    }
    for (int i = 0; i < ATA_MAX_CHANNELS; i++) {
        if (!ata_channels[i].in_use) {
            ata_channels[i].io_base   = io_base;
            ata_channels[i].ctrl_base = ctrl_base;
            ata_channels[i].in_use    = true;
            mutex_init(&ata_channels[i].lock);
            return &ata_channels[i];
        }
    }
    return NULL;
}

static bool ata_device_registered(uint16_t io_base, uint8_t slave) {
    for (uint8_t i = 0; i < ata_device_total; i++) {
        if (ata_devices[i].io_base == io_base && ata_devices[i].slave == slave)
            return true;
    }
    return false;
}

// IDENTIFY DEVICE. Returns true only for an ATA (non-packet) disk and fills
// sector_count. ATAPI (CD-ROM) and SATA-signature devices abort IDENTIFY and
// leave a signature in LBA mid/hi; those are skipped.
static bool ata_identify(uint16_t io, uint8_t slave, uint32_t* sector_count) {
    outb(io + ATA_REG_DRIVE, ATA_DRIVE_CHS(slave));
    ata_delay_400ns(io);

    outb(io + ATA_REG_COUNT,   0);
    outb(io + ATA_REG_LBA_LO,  0);
    outb(io + ATA_REG_LBA_MID, 0);
    outb(io + ATA_REG_LBA_HI,  0);
    outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay_400ns(io);

    // Status 0: no drive in this position
    uint8_t status = inb(io + ATA_REG_STATUS);
    if (status == 0x00 || status == 0xFF)
        return false;

    uint64_t start = timer_get_ms();
    while (inb(io + ATA_REG_STATUS) & ATA_SR_BSY) {
        if (timer_get_ms() - start >= ATA_IDENTIFY_TIMEOUT_MS)
            return false;
    }

    // A non-zero signature means ATAPI (0x14/0xEB) or SATA (0x3C/0xC3)
    if (inb(io + ATA_REG_LBA_MID) != 0 || inb(io + ATA_REG_LBA_HI) != 0)
        return false;

    start = timer_get_ms();
    while (1) {
        status = inb(io + ATA_REG_STATUS);
        if (status & (ATA_SR_ERR | ATA_SR_DF)) return false;
        if (status & ATA_SR_DRQ) break;
        if (timer_get_ms() - start >= ATA_IDENTIFY_TIMEOUT_MS) return false;
    }

    uint16_t identify[256];
    for (int i = 0; i < 256; i++)
        identify[i] = inw(io + ATA_REG_DATA);

    // Words 60-61: total user-addressable sectors in LBA28 mode.
    // Zero means the drive does not support LBA, which this driver needs.
    uint32_t sectors = (uint32_t)identify[60] | ((uint32_t)identify[61] << 16);
    if (sectors == 0)
        return false;

    *sector_count = sectors;
    return true;
}

uint8_t ata_probe_channel(uint16_t io_base, uint16_t ctrl_base) {
    if (io_base == 0)
        return 0;

    // Floating bus: no controller decodes this I/O range at all
    outb(io_base + ATA_REG_DRIVE, ATA_DRIVE_CHS(0));
    ata_delay_400ns(io_base);
    if (inb(io_base + ATA_REG_STATUS) == 0xFF)
        return 0;

    struct AtaChannel* channel = ata_channel_get(io_base, ctrl_base);
    if (channel == NULL)
        return 0;

    // Polled driver: keep the drives from raising INTRQ
    if (ctrl_base != 0)
        outb(ctrl_base, ATA_CTRL_NIEN);

    uint8_t found = 0;

    mutex_lock(&channel->lock);
    for (uint8_t slave = 0; slave < 2; slave++) {
        if (ata_device_total >= ATA_MAX_DEVICES)
            break;
        if (ata_device_registered(io_base, slave))
            continue;

        uint32_t sectors = 0;
        if (!ata_identify(io_base, slave, &sectors))
            continue;

        struct AtaDevice* dev = &ata_devices[ata_device_total];
        dev->channel      = channel;
        dev->io_base      = io_base;
        dev->ctrl_base    = ctrl_base;
        dev->slave        = slave;
        dev->index        = ata_device_total;
        dev->sector_count = sectors;
        ata_device_total++;
        found++;
    }
    mutex_unlock(&channel->lock);

    return found;
}

uint8_t ata_device_count(void) {
    return ata_device_total;
}

struct AtaDevice* ata_get_device(uint8_t index) {
    if (index >= ata_device_total) return NULL;
    return &ata_devices[index];
}

//
// Per-device access
//

// Caller holds the channel lock
static bool ata_issue(struct AtaDevice* dev, uint32_t address, uint8_t command) {
    uint16_t io = dev->io_base;

    // The drive ignores register writes while BSY (e.g. still flushing)
    if (!ata_wait(io, 0, 0, ATA_TIMEOUT_MS)) return false;

    outb(io + ATA_REG_DRIVE, ATA_DRIVE_LBA(dev->slave) | ((address >> 24) & 0x0F));
    ata_delay_400ns(io);

    // Selecting the other drive on a shared channel: wait for it to be ready
    if (!ata_wait(io, 0, 0, ATA_TIMEOUT_MS)) return false;

    outb(io + ATA_REG_COUNT,   1);
    outb(io + ATA_REG_LBA_LO,  (uint8_t)address);
    outb(io + ATA_REG_LBA_MID, (uint8_t)(address >> 8));
    outb(io + ATA_REG_LBA_HI,  (uint8_t)(address >> 16));
    outb(io + ATA_REG_COMMAND, command);
    ata_delay_400ns(io);
    return true;
}

static inline bool ata_lba_valid(struct AtaDevice* dev, uint32_t address) {
    // LBA28: 28-bit address, and never past the end of the disk
    return address < 0x10000000U && address < dev->sector_count;
}

bool ata_dev_read_sector(struct AtaDevice* dev, uint32_t address, uint8_t* buffer) {
    if (dev == NULL || buffer == NULL || !ata_lba_valid(dev, address)) return false;

    uint16_t* word_buf = (uint16_t*)buffer;
    bool ok = false;

    mutex_lock(&dev->channel->lock);
    if (ata_issue(dev, address, ATA_CMD_READ) &&
        ata_wait(dev->io_base, ATA_SR_DRQ, ATA_SR_DRQ, ATA_TIMEOUT_MS)) {
        for (int i = 0; i < 256; i++)
            word_buf[i] = inw(dev->io_base + ATA_REG_DATA);
        ok = true;
    }
    mutex_unlock(&dev->channel->lock);
    return ok;
}

// Writes one sector WITHOUT flushing the drive cache; call ata_dev_flush() at
// sync / shutdown instead.
bool ata_dev_write_sector(struct AtaDevice* dev, uint32_t address, const uint8_t* buffer) {
    if (dev == NULL || buffer == NULL || !ata_lba_valid(dev, address)) return false;

    const uint16_t* word_buf = (const uint16_t*)buffer;
    bool ok = false;

    mutex_lock(&dev->channel->lock);
    if (ata_issue(dev, address, ATA_CMD_WRITE) &&
        ata_wait(dev->io_base, ATA_SR_DRQ, ATA_SR_DRQ, ATA_TIMEOUT_MS)) {
        for (int i = 0; i < 256; i++)
            outw(dev->io_base + ATA_REG_DATA, word_buf[i]);
        ata_delay_400ns(dev->io_base);

        // The drive goes BSY while it takes the data; make sure it accepted it
        ok = ata_wait(dev->io_base, 0, 0, ATA_TIMEOUT_MS);
    }
    mutex_unlock(&dev->channel->lock);
    return ok;
}

bool ata_dev_flush(struct AtaDevice* dev) {
    if (dev == NULL) return false;
    uint16_t io = dev->io_base;
    bool ok = false;

    mutex_lock(&dev->channel->lock);
    if (ata_wait(io, 0, 0, ATA_TIMEOUT_MS)) {
        outb(io + ATA_REG_DRIVE, ATA_DRIVE_LBA(dev->slave));
        ata_delay_400ns(io);
        if (ata_wait(io, 0, 0, ATA_TIMEOUT_MS)) {
            outb(io + ATA_REG_COMMAND, ATA_CMD_FLUSH);
            ata_delay_400ns(io);
            ok = ata_wait(io, 0, 0, ATA_FLUSH_TIMEOUT_MS);
        }
    }
    mutex_unlock(&dev->channel->lock);
    return ok;
}

// Read total_bytes starting at sector `address`. Whole sectors go straight into
// the caller's buffer; a trailing partial sector goes through a bounce buffer,
// so the caller's buffer is never written past total_bytes.
bool ata_dev_read_bytes(struct AtaDevice* dev, uint32_t address, uint8_t* buffer, size_t total_bytes) {
    if (dev == NULL || buffer == NULL || total_bytes == 0)
        return false;

    size_t full_sectors = total_bytes / ATA_SECTOR_SIZE;
    size_t tail_bytes   = total_bytes % ATA_SECTOR_SIZE;

    uint32_t current_lba = address;
    uint8_t* current_buffer_ptr = buffer;

    for (size_t i = 0; i < full_sectors; i++) {
        if (!ata_dev_read_sector(dev, current_lba, current_buffer_ptr))
            return false;
        current_lba++;
        current_buffer_ptr += ATA_SECTOR_SIZE;
    }

    if (tail_bytes > 0) {
        uint8_t bounce[ATA_SECTOR_SIZE];
        if (!ata_dev_read_sector(dev, current_lba, bounce))
            return false;
        memcpy(current_buffer_ptr, bounce, tail_bytes);
    }

    return true;
}

// Write total_bytes starting at sector `address`. A trailing partial sector is
// read-modify-written so the bytes after total_bytes on disk are preserved.
bool ata_dev_write_bytes(struct AtaDevice* dev, uint32_t address, const uint8_t* buffer, size_t total_bytes) {
    if (dev == NULL || buffer == NULL || total_bytes == 0)
        return false;

    size_t full_sectors = total_bytes / ATA_SECTOR_SIZE;
    size_t tail_bytes   = total_bytes % ATA_SECTOR_SIZE;

    uint32_t current_lba = address;
    const uint8_t* current_buffer_ptr = buffer;

    for (size_t i = 0; i < full_sectors; i++) {
        if (!ata_dev_write_sector(dev, current_lba, current_buffer_ptr))
            return false;
        current_lba++;
        current_buffer_ptr += ATA_SECTOR_SIZE;
    }

    if (tail_bytes > 0) {
        uint8_t bounce[ATA_SECTOR_SIZE];
        if (!ata_dev_read_sector(dev, current_lba, bounce))
            return false;
        memcpy(bounce, current_buffer_ptr, tail_bytes);
        if (!ata_dev_write_sector(dev, current_lba, bounce))
            return false;
    }

    return true;
}

//
// Block layer adapter
//

static bool ata_block_read(struct BlockDevice* bd, uint64_t lba, uint32_t count, void* buffer) {
    struct AtaDevice* dev = (struct AtaDevice*)bd->driver_data;
    uint8_t* ptr = (uint8_t*)buffer;

    // The block layer has range-checked lba + count against sector_count,
    // which IDENTIFY capped at LBA28, so the casts cannot truncate
    for (uint32_t i = 0; i < count; i++) {
        if (!ata_dev_read_sector(dev, (uint32_t)(lba + i), ptr))
            return false;
        ptr += ATA_SECTOR_SIZE;
    }
    return true;
}

static bool ata_block_write(struct BlockDevice* bd, uint64_t lba, uint32_t count, const void* buffer) {
    struct AtaDevice* dev = (struct AtaDevice*)bd->driver_data;
    const uint8_t* ptr = (const uint8_t*)buffer;

    for (uint32_t i = 0; i < count; i++) {
        if (!ata_dev_write_sector(dev, (uint32_t)(lba + i), ptr))
            return false;
        ptr += ATA_SECTOR_SIZE;
    }
    return true;
}

static bool ata_block_flush(struct BlockDevice* bd) {
    return ata_dev_flush((struct AtaDevice*)bd->driver_data);
}

static const struct BlockDeviceOps ata_block_ops = {
    .read  = ata_block_read,
    .write = ata_block_write,
    .flush = ata_block_flush,
};

struct BlockDevice* ata_create_block_device(struct AtaDevice* dev) {
    if (dev == NULL) return NULL;
    return block_register("ata", &ata_block_ops, dev, ATA_SECTOR_SIZE, dev->sector_count);
}
