#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/driver/driver.h>
#include <kernel/driver/device.h>
#include <kernel/driver/chardev.h>

#include <kernel/syscall/syscall.h>
#include <kernel/util/string.h>

//
// Memory devices: /dev/null and /dev/zero
//
// The platform device "mem" (added by driver_init). Both files take any
// write and throw it away; reading /dev/null is always at the end, reading
// /dev/zero fills the buffer with zeros. Any process may use them.
//

static int32_t memdev_write(struct CharDevice* cd, const void* buffer, uint32_t size) {
    (void)cd; (void)buffer;
    return (int32_t)size;
}

static int32_t null_read(struct CharDevice* cd, void* buffer, uint32_t size) {
    (void)cd; (void)buffer; (void)size;
    return 0;
}

static int32_t zero_read(struct CharDevice* cd, void* buffer, uint32_t size) {
    (void)cd;
    memset(buffer, 0, size);
    return (int32_t)size;
}

static const struct CharDeviceOps null_ops = {
    .read  = null_read,
    .write = memdev_write,
};

static const struct CharDeviceOps zero_ops = {
    .read  = zero_read,
    .write = memdev_write,
};

struct MemDevices {
    struct CharDevice* null;
    struct CharDevice* zero;
};

static struct MemDevices memdev_state;

static int memdev_probe(struct Device* dev) {
    memdev_state.null = chardev_register("null", &null_ops, NULL, SYS_SEC_NONE);
    memdev_state.zero = chardev_register("zero", &zero_ops, NULL, SYS_SEC_NONE);
    if (memdev_state.null == NULL || memdev_state.zero == NULL) {
        chardev_unregister(memdev_state.null);
        chardev_unregister(memdev_state.zero);
        return PROBE_FAILED;
    }
    dev->driver_data = &memdev_state;
    return PROBE_BOUND;
}

static void memdev_remove(struct Device* dev) {
    struct MemDevices* state = dev->driver_data;
    chardev_unregister(state->null);
    chardev_unregister(state->zero);
    state->null = NULL;
    state->zero = NULL;
}

static const struct DeviceMatch memdev_match[] = {
    { .bus = BUS_PLATFORM, .name = "mem" },
    { 0 }
};

static const struct Driver memdev_driver = {
    .name    = "memdev",
    .abi     = DRIVER_ABI,
    .flags   = DRIVER_BUILTIN,
    .version = "1.0",
    .match   = memdev_match,
    .probe   = memdev_probe,
    .remove  = memdev_remove,
};
DRIVER_BUILTIN_DECLARE(memdev_driver);
