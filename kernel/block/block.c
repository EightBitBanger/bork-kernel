#include <kernel/block/block.h>

// Registration happens during the boot-time bus scan, and at run time for
// removable disks (USB), from one thread at a time. Transfers are not
// locked here: each driver serializes its own hardware, and the filesystem
// serializes each mounted device under the VFS lock.
//
// A BlockDevice never moves while it is registered: the devices live in a
// pool, and block_devices[] lists the ones in use, in registration order.

static struct BlockDevice block_pool[BLOCK_DEVICE_MAX];
static bool block_pool_used[BLOCK_DEVICE_MAX];
static struct BlockDevice* block_devices[BLOCK_DEVICE_MAX];
static uint32_t block_total = 0;

static size_t block_strlen(const char* s) {
    size_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

static bool block_streq(const char* a, const char* b) {
    while (*a != '\0' && *a == *b) { a++; b++; }
    return *a == *b;
}

// "ata" + 3 -> "ata3". Returns false if it does not fit.
static bool block_make_name(char* out, const char* prefix, uint32_t index) {
    size_t len = block_strlen(prefix);

    char digits[10];
    size_t ndigits = 0;
    do {
        digits[ndigits++] = (char)('0' + (index % 10));
        index /= 10;
    } while (index != 0 && ndigits < sizeof(digits));

    if (len + ndigits + 1 > BLOCK_NAME_MAX)
        return false;

    for (size_t i = 0; i < len; i++)
        out[i] = prefix[i];
    for (size_t i = 0; i < ndigits; i++)
        out[len + i] = digits[ndigits - 1 - i];
    out[len + ndigits] = '\0';
    return true;
}

// Lowest number not taken by a device named prefix + number. Devices come
// and go (USB), so counting them could hand out a name still in use.
static uint32_t block_prefix_free(const char* prefix) {
    char name[BLOCK_NAME_MAX];
    for (uint32_t n = 0; n < BLOCK_DEVICE_MAX; n++) {
        if (block_make_name(name, prefix, n) && block_find(name) == NULL)
            return n;
    }
    return BLOCK_DEVICE_MAX;
}

static struct BlockDevice* block_add(const char* name, const struct BlockDeviceOps* ops,
                                     void* driver_data, uint32_t sector_size, uint64_t sector_count) {
    if (block_total >= BLOCK_DEVICE_MAX)
        return NULL;

    struct BlockDevice* dev = NULL;
    for (uint32_t slot = 0; slot < BLOCK_DEVICE_MAX && dev == NULL; slot++) {
        if (!block_pool_used[slot]) {
            block_pool_used[slot] = true;
            dev = &block_pool[slot];
        }
    }
    if (dev == NULL)
        return NULL;

    size_t i = 0;
    for (; name[i] != '\0'; i++)
        dev->name[i] = name[i];
    dev->name[i] = '\0';

    dev->sector_size  = sector_size;
    dev->sector_count = sector_count;
    dev->ops          = ops;
    dev->driver_data  = driver_data;

    block_devices[block_total++] = dev;
    return dev;
}

void block_unregister(struct BlockDevice* dev) {
    for (uint32_t i = 0; i < block_total; i++) {
        if (block_devices[i] != dev)
            continue;
        for (; i + 1 < block_total; i++)
            block_devices[i] = block_devices[i + 1];
        block_devices[--block_total] = NULL;

        uint32_t slot = (uint32_t)(dev - block_pool);
        dev->name[0]      = '\0';
        dev->ops          = NULL;
        dev->driver_data  = NULL;
        dev->sector_count = 0;
        block_pool_used[slot] = false;
        return;
    }
}

static bool block_args_valid(const struct BlockDeviceOps* ops, uint32_t sector_size, uint64_t sector_count) {
    return ops != NULL && ops->read != NULL && ops->write != NULL &&
           sector_size != 0 && sector_count != 0;
}

struct BlockDevice* block_register(const char* prefix, const struct BlockDeviceOps* ops,
                                   void* driver_data, uint32_t sector_size, uint64_t sector_count) {
    if (prefix == NULL || !block_args_valid(ops, sector_size, sector_count))
        return NULL;

    struct BlockDevice* existing = block_find_driver(driver_data);
    if (existing != NULL)
        return existing;

    char name[BLOCK_NAME_MAX];
    if (!block_make_name(name, prefix, block_prefix_free(prefix)))
        return NULL;

    return block_add(name, ops, driver_data, sector_size, sector_count);
}

struct BlockDevice* block_register_named(const char* name, const struct BlockDeviceOps* ops,
                                         void* driver_data, uint32_t sector_size, uint64_t sector_count) {
    if (name == NULL || name[0] == '\0' || !block_args_valid(ops, sector_size, sector_count))
        return NULL;
    if (block_strlen(name) >= BLOCK_NAME_MAX)
        return NULL;

    struct BlockDevice* existing = block_find_driver(driver_data);
    if (existing != NULL)
        return existing;

    if (block_find(name) != NULL)
        return NULL;   // Name taken by a different device

    return block_add(name, ops, driver_data, sector_size, sector_count);
}

uint32_t block_device_count(void) {
    return block_total;
}

struct BlockDevice* block_get(uint32_t index) {
    if (index >= block_total) return NULL;
    return block_devices[index];
}

struct BlockDevice* block_find(const char* name) {
    if (name == NULL) return NULL;
    for (uint32_t i = 0; i < block_total; i++) {
        if (block_streq(block_devices[i]->name, name))
            return block_devices[i];
    }
    return NULL;
}

struct BlockDevice* block_find_driver(const void* driver_data) {
    if (driver_data == NULL) return NULL;
    for (uint32_t i = 0; i < block_total; i++) {
        if (block_devices[i]->driver_data == driver_data)
            return block_devices[i];
    }
    return NULL;
}

static bool block_range_valid(const struct BlockDevice* dev, uint64_t lba, uint32_t count) {
    if (dev == NULL || dev->ops == NULL || count == 0) return false;
    if (lba >= dev->sector_count) return false;
    return (uint64_t)count <= dev->sector_count - lba;
}

bool block_read(struct BlockDevice* dev, uint64_t lba, uint32_t count, void* buffer) {
    if (buffer == NULL || !block_range_valid(dev, lba, count)) return false;
    return dev->ops->read(dev, lba, count, buffer);
}

bool block_write(struct BlockDevice* dev, uint64_t lba, uint32_t count, const void* buffer) {
    if (buffer == NULL || !block_range_valid(dev, lba, count)) return false;
    return dev->ops->write(dev, lba, count, buffer);
}

bool block_flush(struct BlockDevice* dev) {
    if (dev == NULL || dev->ops == NULL) return false;
    if (dev->ops->flush == NULL) return true;   // No write cache to commit
    return dev->ops->flush(dev);
}

uint64_t block_capacity_bytes(const struct BlockDevice* dev) {
    if (dev == NULL) return 0;
    return dev->sector_count * (uint64_t)dev->sector_size;
}

// Zeroes for block_zero. Static, so a DMA driver can map it.
#define BLOCK_ZERO_SECTORS  64
#define BLOCK_ZERO_BYTES    (BLOCK_ZERO_SECTORS * 512)
static uint8_t block_zeroes[BLOCK_ZERO_BYTES] __attribute__((aligned(16)));

bool block_zero(struct BlockDevice* dev, uint64_t lba, uint64_t count,
                void (*progress)(uint32_t percent)) {
    if (dev == NULL || dev->sector_size == 0 || dev->sector_size > BLOCK_ZERO_BYTES)
        return false;
    if (lba > dev->sector_count || count > dev->sector_count - lba)
        return false;

    uint32_t chunk   = BLOCK_ZERO_BYTES / dev->sector_size;
    uint32_t percent = 0;

    for (uint64_t done = 0; done < count; ) {
        uint32_t n = (count - done < chunk) ? (uint32_t)(count - done) : chunk;
        if (!block_write(dev, lba + done, n, block_zeroes))
            return false;
        done += n;

        uint32_t now = (uint32_t)((done * 100) / count);
        if (progress != NULL && now != percent) {
            percent = now;
            progress(percent);
        }
    }
    return true;
}
