#ifndef _KERNEL_DEVICE_H_
#define _KERNEL_DEVICE_H_

// Devices
//
// One piece of hardware (or a virtual device) a bus found. Buses add them;
// the driver core offers each one to the registered drivers and binds it
// to the first whose probe() says PROBE_BOUND. A device nobody takes stays
// listed and is offered again to every driver registered later.
//
// A device lives as long as the hardware does: the bus calls device_remove
// when it goes away, which calls the driver's remove() first.
//
// Buses: see the lock order in driver.h. device_add and device_remove take
// the driver lock, so never call them with a bus lock held.

#include <stdint.h>
#include <stdbool.h>

#include <kernel/driver/driver.h>

#define DEVICE_NAME_MAX     16      // Including the terminator
#define DEVICE_MAX          64      // Devices known at once

struct Device {
    char     name[DEVICE_NAME_MAX]; // "mem", "pci0:1f.2", "usb1-3:0"
    uint8_t  bus;                   // BUS_*
    struct Device* parent;          // Controller / hub / bridge, NULL at the root

    // Copied from the bus, so matching needs no bus code (MATCH_ANY when
    // the bus has no such number)
    uint16_t vendor;
    uint16_t product;
    uint16_t class_code;
    uint16_t subclass;
    uint16_t protocol;

    void*    bus_data;              // The bus's own record (PCIDeviceInfo*, UsbDevice*...)
    uint8_t  bus_index;             // USB: interface number. Otherwise 0

    const struct Driver* driver;    // NULL while nothing holds it
    void*    driver_data;           // The driver's state for this device
};

// What a bus knows about a device it adds. Numbers it has none of are
// MATCH_ANY.
struct DeviceInfo {
    uint8_t  bus;
    uint16_t vendor;
    uint16_t product;
    uint16_t class_code;
    uint16_t subclass;
    uint16_t protocol;
    void*    bus_data;
    uint8_t  bus_index;
};

// Add a device and offer it to the drivers. Returns it whether or not a
// driver took it; NULL when the name is empty or taken, or the table is
// full.
struct Device* device_add(const char* name, struct Device* parent, const struct DeviceInfo* info);

// A virtual or fixed device on the platform bus, matched by name
struct Device* device_add_platform(const char* name);

// The device is gone: the devices below it go first, then its driver's
// remove() runs and it is forgotten. `dev` is freed.
void device_remove(struct Device* dev);

// device_count / device_get read the table without the driver lock: fine
// for listing, but a device may be removed right after (hot plug)
uint32_t       device_count(void);
struct Device* device_get(uint32_t index);
struct Device* device_find(const char* name);

// Name of a bus for printing: "platform", "pci", "usb"
const char* device_bus_name(uint8_t bus);

#endif
