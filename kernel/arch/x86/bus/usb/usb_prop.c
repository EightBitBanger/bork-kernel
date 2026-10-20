#include <kernel/arch/x86/heap.h>
#include <kernel/arch/x86/bus/usb/usb.h>
#include <kernel/arch/x86/bus/usb/usb_internal.h>

#include <kernel/kernel.h>
#include <kernel/knode.h>
#include <kernel/device/device.h>

#include <kernel/util/string.h>

// /dev/usb/dev<n>/<property> -> device node whose name is the value, the
// same layout as /dev/pci. n is the device's index in usb_get_device().

static void* usb_directory = NULL;

static void* usb_get_directory(void) {
    if (usb_directory != NULL)
        return usb_directory;

    void* dev_directory = knode_find_by_name(knode_get_root(), "dev");
    if (dev_directory == NULL)
        return NULL;

    usb_directory = knode_find_by_name(dev_directory, "usb");
    if (usb_directory == NULL) {
        usb_directory = create_knode("usb", dev_directory);
        knode_set_permissions(usb_directory, KMALLOC_PERMISSION_READ);
    }
    return usb_directory;
}

static void usb_property(void* parent, const char* name, const char* value) {
    // Device names hold 15 characters
    char text[sizeof(((struct Device*)0)->name)];
    strncpy(text, value, sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';

    void* prop_dir = create_knode(name, parent);
    struct Device* device_val = create_device(text);
    knode_add_reference(prop_dir, device_val);
    knode_set_permissions(prop_dir, KMALLOC_PERMISSION_READ);
    knode_set_permissions(device_val, KMALLOC_PERMISSION_READ);
}

static void usb_property_hex16(void* parent, const char* name, uint16_t value) {
    char buf[8] = "0x";
    u16tox(value, &buf[2]);
    usb_property(parent, name, buf);
}

static void usb_property_dec(void* parent, const char* name, uint32_t value) {
    char buf[12];
    utos(value, buf);
    usb_property(parent, name, buf);
}

static int32_t usb_device_index(const struct UsbDevice* dev) {
    for (uint32_t i = 0; i < usb_device_count(); i++) {
        if (usb_get_device(i) == dev) return (int32_t)i;
    }
    return -1;
}

void usb_export_device(struct UsbDevice* dev) {
    void* dir = usb_get_directory();
    int32_t index = usb_device_index(dev);
    if (dir == NULL || index < 0)
        return;

    char name[KNODE_NAME_LENGTH_MAX] = "dev";
    utos((uint32_t)index, &name[3]);

    void* node = create_knode(name, dir);
    knode_set_permissions(node, KMALLOC_PERMISSION_READ);

    const struct UsbDeviceDescriptor* d = &dev->descriptor;
    usb_property_hex16(node, "vendor",  d->vendor_id);
    usb_property_hex16(node, "product", d->product_id);
    usb_property_hex16(node, "usb",     d->usb_version);

    // Class from the device descriptor, or from the first interface when
    // the device defers to its interfaces
    bool per_interface = d->device_class == USB_CLASS_PER_INTERFACE;
    usb_property(node,     "class",    usb_class_name(per_interface ? dev->if_class : d->device_class));
    usb_property_dec(node, "subclass", per_interface ? dev->if_subclass : d->device_subclass);
    usb_property_dec(node, "protocol", per_interface ? dev->if_protocol : d->device_protocol);

    usb_property(node, "speed", usb_speed_name(dev->speed));
    usb_property_dec(node, "address", dev->address);
    usb_property_dec(node, "port", dev->port);

    // "root" or the hub's dev<n>
    if (dev->parent == NULL) {
        usb_property(node, "hub", "root");
    } else {
        char hub[KNODE_NAME_LENGTH_MAX] = "dev";
        utos((uint32_t)usb_device_index(dev->parent), &hub[3]);
        usb_property(node, "hub", hub);
    }

    // Controller, e.g. "xhci0"
    char hc_name[16];
    strncpy(hc_name, usb_controller_name(dev->hc), 8);
    hc_name[8] = '\0';
    utos(usb_controller_index(dev->hc), &hc_name[strlen(hc_name)]);
    usb_property(node, "controller", hc_name);

    if (dev->manufacturer[0] != '\0') usb_property(node, "maker", dev->manufacturer);
    if (dev->product[0] != '\0')      usb_property(node, "name",  dev->product);
    if (dev->serial[0] != '\0')       usb_property(node, "serial", dev->serial);
}
