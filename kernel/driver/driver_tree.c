#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/driver/driver.h>
#include <kernel/driver/device.h>
#include <kernel/driver/driver_internal.h>

#include <kernel/kernel.h>
#include <kernel/knode.h>
#include <kernel/vfs/vfs.h>
#include <kernel/util/string.h>

// /drv: every registered driver and the devices it holds
//
//   /drv/<driver>/version/<"1.0">
//                /type/<"device" | "library" | "service">
//                /bus/<"platform" | "pci" | "usb" | "none">
//                /origin/<"builtin" | "loaded">
//                /state/<"bound" | "idle" | "running">
//                /devices/<device name>       one entry per device it holds
//                /exports/<function name>     libraries: one entry per function
//
// The same layout as /dev/pci: a property is a folder holding one entry
// whose name is the value, so "list /drv/memdev/state" shows it. Rebuilt
// whole on every change; there are few drivers.

static void* drv_directory = NULL;

static void* drv_get_directory(void) {
    if (drv_directory != NULL)
        return drv_directory;
    void* root = knode_get_root();
    if (root == NULL)
        return NULL;
    drv_directory = knode_find_by_name(root, "drv");
    if (drv_directory == NULL) {
        drv_directory = create_knode("drv", root);
        if (drv_directory != NULL)
            knode_set_permissions(drv_directory, KMALLOC_PERMISSION_READ);
    }
    return drv_directory;
}

// Free a knode folder and everything below it. Handles open on any of it
// are revoked first (VFS lock held).
static void drv_destroy_tree(void* node, void* parent) {
    for (uint32_t n = knode_get_reference_count(node); n > 0; n--) {
        void* child = knode_get_reference(node, n - 1);
        if (knode_check_is_valid(child)) {
            drv_destroy_tree(child, node);
        } else {
            vfs_knode_revoke_handles(child);
            knode_remove_reference(node, child);
            destroy_property(child);
        }
    }
    vfs_knode_revoke_handles(node);
    destroy_knode(node, parent);
}

// An entry named `value` in `parent`. Values longer than a knode name are cut.
static void drv_value(void* parent, const char* value) {
    struct KProperty* property = create_property(value);
    if (property == NULL)
        return;
    knode_set_permissions(property, KMALLOC_PERMISSION_READ);
    if (!knode_add_reference(parent, property))
        destroy_property(property);
}

static void* drv_folder(void* parent, const char* name) {
    void* folder = create_knode(name, parent);
    if (folder != NULL)
        knode_set_permissions(folder, KMALLOC_PERMISSION_READ);
    return folder;
}

static void drv_property(void* parent, const char* name, const char* value) {
    void* folder = drv_folder(parent, name);
    if (folder != NULL)
        drv_value(folder, value);
}

static void drv_export_driver(void* directory, const struct Driver* drv) {
    void* node = drv_folder(directory, drv->name);
    if (node == NULL)
        return;

    uint8_t bus = (drv->match != NULL) ? drv->match[0].bus : 0;
    drv_property(node, "version", (drv->version != NULL) ? drv->version : "-");
    drv_property(node, "type",    driver_type_name(drv->type));
    drv_property(node, "bus",     (drv->type == DRIVER_TYPE_DEVICE) ? device_bus_name(bus) : "none");
    drv_property(node, "origin",  (drv->flags & DRIVER_BUILTIN) ? "builtin" : "loaded");

    if (drv->type == DRIVER_TYPE_LIBRARY && drv->exports != NULL) {
        void* exports = drv_folder(node, "exports");
        for (const struct DriverExport* row = drv->exports; exports != NULL && row->name != NULL; row++)
            drv_value(exports, row->name);
    }

    void* devices = drv_folder(node, "devices");
    uint32_t held = 0;
    for (uint32_t i = 0; i < device_count(); i++) {
        struct Device* dev = device_get(i);
        if (dev->driver != drv)
            continue;
        if (devices != NULL)
            drv_value(devices, dev->name);
        held++;
    }

    // Libraries and services have no devices: they are up once registered
    if (drv->type != DRIVER_TYPE_DEVICE)
        drv_property(node, "state", "running");
    else
        drv_property(node, "state", (held > 0) ? "bound" : "idle");
}

void driver_tree_update(void) {
    vfs_lock();
    void* directory = drv_get_directory();
    if (directory == NULL) {
        vfs_unlock();
        return;     // Before kernel_init(): nothing to show it in yet
    }

    for (uint32_t n = knode_get_reference_count(directory); n > 0; n--)
        drv_destroy_tree(knode_get_reference(directory, n - 1), directory);

    for (uint32_t i = 0; i < driver_count(); i++)
        drv_export_driver(directory, driver_get(i));
    vfs_unlock();
}
