#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/driver/driver.h>
#include <kernel/driver/device.h>
#include <kernel/driver/driver_internal.h>

#include <kernel/malloc.h>
#include <kernel/util/string.h>

// In the order they were added. Only the driver lock guards the table.
static struct Device* devices[DEVICE_MAX];
static uint32_t devices_count = 0;

//
// Binding (driver lock held)
//

// Probe `dev` with `drv`. Returns PROBE_*; bound on PROBE_BOUND.
static int device_probe_with(struct Device* dev, const struct Driver* drv) {
    dev->driver_data = NULL;
    int result = (drv->probe != NULL) ? drv->probe(dev) : PROBE_NOT_MINE;
    if (result == PROBE_BOUND)
        dev->driver = drv;
    else
        dev->driver_data = NULL;
    return result;
}

void device_offer(struct Device* dev) {
    if (dev->driver != NULL)
        return;
    for (uint32_t i = 0; i < driver_count(); i++) {
        const struct Driver* drv = driver_get(i);
        if (!driver_matches(drv, dev))
            continue;
        int result = device_probe_with(dev, drv);
        // PROBE_FAILED: it was the right driver, another one won't do better
        if (result == PROBE_BOUND || result == PROBE_FAILED)
            return;
    }
}

void device_offer_all(const struct Driver* drv) {
    for (uint32_t i = 0; i < devices_count; i++) {
        struct Device* dev = devices[i];
        if (dev->driver == NULL && driver_matches(drv, dev))
            device_probe_with(dev, drv);
    }
}

static void device_unbind(struct Device* dev) {
    if (dev->driver == NULL)
        return;
    if (dev->driver->remove != NULL)
        dev->driver->remove(dev);
    dev->driver = NULL;
    dev->driver_data = NULL;
}

void device_release_all(const struct Driver* drv) {
    for (uint32_t i = 0; i < devices_count; i++) {
        if (devices[i]->driver == drv)
            device_unbind(devices[i]);
    }
}

//
// Devices
//

static struct Device* device_find_locked(const char* name) {
    for (uint32_t i = 0; i < devices_count; i++) {
        if (strncmp(devices[i]->name, name, DEVICE_NAME_MAX) == 0)
            return devices[i];
    }
    return NULL;
}

struct Device* device_add(const char* name, struct Device* parent, const struct DeviceInfo* info) {
    if (name == NULL || name[0] == '\0' || strlen(name) >= DEVICE_NAME_MAX || info == NULL)
        return NULL;

    struct Device* dev = (struct Device*)malloc(sizeof(struct Device));
    if (dev == NULL)
        return NULL;
    memset(dev, 0, sizeof(*dev));

    strncpy(dev->name, name, DEVICE_NAME_MAX - 1);
    dev->bus        = info->bus;
    dev->parent     = parent;
    dev->vendor     = info->vendor;
    dev->product    = info->product;
    dev->class_code = info->class_code;
    dev->subclass   = info->subclass;
    dev->protocol   = info->protocol;
    dev->bus_data   = info->bus_data;
    dev->bus_index  = info->bus_index;

    driver_lock();
    if (devices_count >= DEVICE_MAX || device_find_locked(name) != NULL) {
        driver_unlock();
        free(dev);
        return NULL;
    }
    devices[devices_count++] = dev;

    device_offer(dev);
    driver_tree_update();
    driver_unlock();
    return dev;
}

struct Device* device_add_platform(const char* name) {
    struct DeviceInfo info = {
        .bus        = BUS_PLATFORM,
        .vendor     = MATCH_ANY,
        .product    = MATCH_ANY,
        .class_code = MATCH_ANY,
        .subclass   = MATCH_ANY,
        .protocol   = MATCH_ANY,
    };
    return device_add(name, NULL, &info);
}

static void device_remove_locked(struct Device* dev) {
    // Everything below it goes first. Restart the scan after each removal:
    // the table shifts.
    bool found = true;
    while (found) {
        found = false;
        for (uint32_t i = 0; i < devices_count; i++) {
            if (devices[i]->parent == dev) {
                device_remove_locked(devices[i]);
                found = true;
                break;
            }
        }
    }

    device_unbind(dev);

    for (uint32_t i = 0; i < devices_count; i++) {
        if (devices[i] != dev)
            continue;
        for (uint32_t j = i; j + 1 < devices_count; j++)
            devices[j] = devices[j + 1];
        devices[--devices_count] = NULL;
        break;
    }
    free(dev);
}

void device_remove(struct Device* dev) {
    if (dev == NULL)
        return;
    driver_lock();
    device_remove_locked(dev);
    driver_tree_update();
    driver_unlock();
}

uint32_t device_count(void) {
    return devices_count;
}

struct Device* device_get(uint32_t index) {
    return (index < devices_count) ? devices[index] : NULL;
}

struct Device* device_find(const char* name) {
    if (name == NULL)
        return NULL;
    driver_lock();
    struct Device* dev = device_find_locked(name);
    driver_unlock();
    return dev;
}

const char* device_bus_name(uint8_t bus) {
    switch (bus) {
        case BUS_PLATFORM: return "platform";
        case BUS_PCI:      return "pci";
        case BUS_USB:      return "usb";
        default:           return "unknown";
    }
}
