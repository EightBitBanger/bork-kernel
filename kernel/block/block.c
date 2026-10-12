#include <kernel/block/block.h>

// Registration happens during the boot-time bus scan, from one thread.
// Transfers are not locked here: each driver serializes its own hardware,
// and the filesystem serializes each mounted device under the VFS lock.

static struct BlockDevice block_devices[BLOCK_DEVICE_MAX];
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

// How many registered devices are named exactly this prefix followed by
// digits. The whole tail must be digits: a partition such as "ata0p1"
// starts with "ata" and a digit, but it is not an "ata" disk, and counting
// it would number the next disk "ata2" instead of "ata1".
static uint32_t block_prefix_count(const char* prefix) {
    size_t len = block_strlen(prefix);
    uint32_t count = 0;

    for (uint32_t i = 0; i < block_total; i++) {
        const char* name = block_devices[i].name;
        size_t j = 0;
        while (j < len && name[j] == prefix[j]) j++;
        if (j != len || name[j] < '0' || name[j] > '9')
            continue;
        while (name[j] >= '0' && name[j] <= '9') j++;
        if (name[j] != '\0')
            continue;
        count++;
    }
    return count;
}

static struct BlockDevice* block_add(const char* name, const struct BlockDeviceOps* ops,
                                     void* driver_data, uint32_t sector_size, uint64_t sector_count) {
    if (block_total >= BLOCK_DEVICE_MAX)
        return NULL;

    struct BlockDevice* dev = &block_devices[block_total];
    size_t i = 0;
    for (; name[i] != '\0'; i++)
        dev->name[i] = name[i];
    dev->name[i] = '\0';

    dev->sector_size  = sector_size;
    dev->sector_count = sector_count;
    dev->ops          = ops;
    dev->driver_data  = driver_data;

    block_total++;
    return dev;
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
    if (!block_make_name(name, prefix, block_prefix_count(prefix)))
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
    return &block_devices[index];
}

struct BlockDevice* block_find(const char* name) {
    if (name == NULL) return NULL;
    for (uint32_t i = 0; i < block_total; i++) {
        if (block_streq(block_devices[i].name, name))
            return &block_devices[i];
    }
    return NULL;
}

struct BlockDevice* block_find_driver(const void* driver_data) {
    if (driver_data == NULL) return NULL;
    for (uint32_t i = 0; i < block_total; i++) {
        if (block_devices[i].driver_data == driver_data)
            return &block_devices[i];
    }
    return NULL;
}

static bool block_range_valid(const struct BlockDevice* dev, uint64_t lba, uint32_t count) {
    if (dev == NULL || count == 0) return false;
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
    if (dev == NULL) return false;
    if (dev->ops->flush == NULL) return true;   // No write cache to commit
    return dev->ops->flush(dev);
}

uint64_t block_capacity_bytes(const struct BlockDevice* dev) {
    if (dev == NULL) return 0;
    return dev->sector_count * (uint64_t)dev->sector_size;
}
