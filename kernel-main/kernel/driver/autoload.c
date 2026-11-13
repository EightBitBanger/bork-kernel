// Driver files for hardware nothing took (autoload.h)

#include <stdint.h>
#include <stdbool.h>

#include <kernel/driver/autoload.h>
#include <kernel/driver/driver.h>
#include <kernel/driver/device.h>
#include <kernel/driver/driver_internal.h>
#include <kernel/driver/kapi.h>
#include <kernel/driver/kdrv_loader.h>
#include <kernel/arch/x86/drivers/serial.h>
#include <kernel/kernel.h>
#include <kernel/syscall/syscall.h>
#include <kernel/vfs/vfs.h>
#include <kernel/util/snprint.h>
#include <kernel/util/string.h>
#include <kernel/util/timer.h>

#define AUTOLOAD_POLL_MS    500         // How often it looks (for the disk, then for new devices)
#define AUTOLOAD_TRIED_MAX  DEVICE_MAX

// A class of device and the file that drives it
struct AutoloadClass {
    uint8_t     bus;
    uint16_t    class_code;
    uint16_t    subclass;               // MATCH_ANY: any
    const char* file;
};

static const struct AutoloadClass autoload_classes[] = {
    { BUS_PCI, 0x04, 0x03, "hda" },     // Multimedia / HD Audio (sys/drivers/hda)
};

#define AUTOLOAD_CLASS_COUNT (sizeof(autoload_classes) / sizeof(autoload_classes[0]))

// One device by its IDs and the file that drives it. Only the names: what
// the file does with the device is the file's business.
struct AutoloadId {
    uint8_t     bus;
    uint16_t    vendor;
    uint16_t    product;
    const char* file;
};

#include <drivers/rtw-usb/rtw_devices.h>    // The Realtek sticks, the driver's own list
#define AUTOLOAD_RTW_ROW(v, p, f)   { BUS_USB, (v), (p), "rtw-usb" },

static const struct AutoloadId autoload_ids[] = {
    { BUS_PCI, 0x168C, 0x0032, "athn-pci" },    // Atheros AR9485 Wi-Fi (sys/drivers/athn-pci)
    RTW_DEVICES(AUTOLOAD_RTW_ROW)               // Realtek USB Wi-Fi (sys/drivers/rtw-usb)
    { BUS_USB, RTW_ZEROCD_VENDOR, RTW_ZEROCD_PRODUCT, "rtw-usb" },
};

#define AUTOLOAD_ID_COUNT   (sizeof(autoload_ids) / sizeof(autoload_ids[0]))

static void autoload_log(const char* path, const char* what) {
    serial_puts("autoload: ");
    serial_puts(path);
    serial_puts(": ");
    serial_puts(what);
    serial_puts("\n");
}

// Only an admin may change the file and the folder it is in
static bool admin_only(const char* path) {
    uint32_t read = 0, write = 0;
    if (!vfs_get_security(path, &read) || !vfs_get_write_security(path, &write) ||
        !((read | write) & SYS_SEC_ADMIN))
        return false;
    char dir[VFS_PATH_MAX];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char* slash = strrchr(dir, '/');
    if (slash == NULL || slash == dir)
        return false;
    *slash = '\0';
    read = write = 0;
    return vfs_get_security(dir, &read) && vfs_get_write_security(dir, &write) &&
           ((read | write) & SYS_SEC_ADMIN);
}

int32_t driver_autoload_file(const char* name) {
    if (name == NULL || name[0] == '\0' || strlen(name) >= DRIVER_NAME_MAX || strchr(name, '/') != NULL)
        return KAPI_LOAD_NO_FILE;
    struct LocalPaths paths;
    kernel_get_local_paths(&paths);
    if (paths.home[0] != '/' || !vfs_directory_check(paths.home))
        return KAPI_LOAD_NO_HOME;
    if (driver_find(name) != NULL)
        return KAPI_LOAD_LOADED;

    char path[VFS_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/sys/drivers/%s", paths.home, name);
    if (n <= 0 || n >= (int)sizeof(path))
        return KAPI_LOAD_NO_FILE;
    FSFileStats st;
    if (!vfs_stat(path, &st) || st.type != VFS_TYPE_FILE)
        return KAPI_LOAD_NO_FILE;
    if (!admin_only(path)) {
        autoload_log(path, "not loaded, users can change it (needs write=admin)");
        return KAPI_LOAD_UNSAFE;
    }
    KdrvResult r = kdrv_load(path, NULL);
    autoload_log(path, r == KDRV_OK ? "loaded" : kdrv_error_string(r));
    if (r == KDRV_OK)
        return KAPI_LOAD_OK;
    return r == KDRV_ERR_EXISTS ? KAPI_LOAD_LOADED : KAPI_LOAD_FAILED;
}

//
// The service
//

// Devices already offered their files (by address and name, so a device
// removed and another added at the same address is offered again)
static struct {
    const struct Device* dev;
    char                 name[DEVICE_NAME_MAX];
} tried[AUTOLOAD_TRIED_MAX];
static uint32_t tried_count;

static bool was_tried(const struct Device* dev) {
    for (uint32_t i = 0; i < tried_count; i++)
        if (tried[i].dev == dev && strncmp(tried[i].name, dev->name, DEVICE_NAME_MAX) == 0)
            return true;
    return false;
}

// `dev` is only kept as an address, never read: `name` is its name
static void mark_tried(const struct Device* dev, const char* name) {
    if (tried_count == AUTOLOAD_TRIED_MAX)
        tried_count = 0;                // Forget the oldest round: at worst a second look
    tried[tried_count].dev = dev;
    strncpy(tried[tried_count].name, name, DEVICE_NAME_MAX - 1);
    tried[tried_count].name[DEVICE_NAME_MAX - 1] = '\0';
    tried_count++;
}

static void add_name(char names[][DRIVER_NAME_MAX], uint32_t* count, uint32_t max, const char* name) {
    for (uint32_t i = 0; i < *count; i++)
        if (strncmp(names[i], name, DRIVER_NAME_MAX) == 0)
            return;
    if (*count == max)
        return;
    strncpy(names[*count], name, DRIVER_NAME_MAX - 1);
    names[*count][DRIVER_NAME_MAX - 1] = '\0';
    (*count)++;
}

// Whether something holds the device of that name now (or it went away)
static bool taken(const char* name) {
    driver_lock();
    struct Device* dev = device_find(name);
    bool yes = dev == NULL || dev->driver != NULL;
    driver_unlock();
    return yes;
}

// The files for one device, most particular first; false: the disk isn't
// there yet. `dev` is a copy: the device itself may be unplugged meanwhile.
#define AUTOLOAD_NAMES_MAX  4
static bool offer(const struct Device* dev) {
    char names[AUTOLOAD_NAMES_MAX][DRIVER_NAME_MAX];
    uint32_t count = 0;
    if (dev->vendor != MATCH_ANY && dev->product != MATCH_ANY) {
        char name[DRIVER_NAME_MAX];
        snprintf(name, sizeof(name), "%s-%04x%04x", dev->bus == BUS_PCI ? "pci" : "usb",
                 (unsigned)dev->vendor, (unsigned)dev->product);
        add_name(names, &count, AUTOLOAD_NAMES_MAX, name);
        for (uint32_t i = 0; i < AUTOLOAD_ID_COUNT; i++) {
            const struct AutoloadId* id = &autoload_ids[i];
            if (id->bus == dev->bus && id->vendor == dev->vendor && id->product == dev->product)
                add_name(names, &count, AUTOLOAD_NAMES_MAX, id->file);
        }
    }
    for (uint32_t i = 0; i < AUTOLOAD_CLASS_COUNT; i++) {
        const struct AutoloadClass* c = &autoload_classes[i];
        if (c->bus == dev->bus && c->class_code == dev->class_code &&
            (c->subclass == MATCH_ANY || c->subclass == dev->subclass))
            add_name(names, &count, AUTOLOAD_NAMES_MAX, c->file);
    }
    for (uint32_t i = 0; i < count; i++) {
        int32_t r = driver_autoload_file(names[i]);
        if (r == KAPI_LOAD_NO_HOME)
            return false;
        if (taken(dev->name))
            break;                      // Taken by what just loaded
    }
    return true;
}

static void autoload_poll(void) {
    struct LocalPaths paths;
    kernel_get_local_paths(&paths);
    if (paths.home[0] != '/' || !vfs_directory_check(paths.home))
        return;                         // No system disk yet
    for (uint32_t i = 0; ; i++) {
        // A copy, taken with the table held still: loading reads files, and
        // a USB device can be pulled out while it does
        struct Device copy;
        const struct Device* dev;
        bool want = false;
        driver_lock();
        bool end = i >= device_count();
        dev = end ? NULL : device_get(i);
        if (dev != NULL && (dev->bus == BUS_PCI || dev->bus == BUS_USB) && !was_tried(dev)) {
            if (dev->driver == NULL) {
                copy = *dev;
                want = true;
            } else {
                // Held already (built in, or by a file another interface of
                // the same stick loaded): that was its look, so "unload"
                // later is not undone by loading the file again
                mark_tried(dev, dev->name);
            }
        }
        driver_unlock();
        if (end)
            break;
        if (!want)
            continue;
        if (!offer(&copy))
            return;
        mark_tried(dev, copy.name);
    }
}

static const struct Driver autoload_driver = {
    .name    = "autoload",
    .abi     = DRIVER_ABI,
    .flags   = DRIVER_BUILTIN,
    .version = "1.0",
    .type    = DRIVER_TYPE_SERVICE,
    .poll    = autoload_poll,
    .poll_ms = AUTOLOAD_POLL_MS,
};
DRIVER_BUILTIN_DECLARE(autoload_driver);
