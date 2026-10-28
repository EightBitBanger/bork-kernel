#include <kernel/arch/x86/heap.h>
#include <kernel/arch/x86/bus/usb/usb.h>
#include <kernel/arch/x86/bus/usb/usb_internal.h>

#include <kernel/kernel.h>
#include <kernel/knode.h>
#include <kernel/device/device.h>

#include <kernel/util/string.h>

// /dev/usb/<name>/<property> -> device node whose name is the value, the
// same layout as /dev/pci. <name> is the device's product string ("Cruzer_Blade"),
// or what it is ("keyboard", "mouse", "hub") when it has none; a second device
// of the same name gets "-2", "-3" ...

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

// What to call a device with no product string
static const char* usb_kind_name(const struct UsbDevice* dev) {
    const struct UsbDeviceDescriptor* d = &dev->descriptor;
    bool per_interface = d->device_class == USB_CLASS_PER_INTERFACE;
    uint8_t class_code = per_interface ? dev->if_class : d->device_class;
    if (class_code == USB_CLASS_HID && dev->if_subclass == 1) {
        if (dev->if_protocol == 1) return "keyboard";
        if (dev->if_protocol == 2) return "mouse";
    }
    return usb_class_name(class_code);
}

// Is the word at `text` (up to a space) "USB", any case
static bool usb_word_is_usb(const char* text) {
    return (text[0] | 0x20) == 'u' && (text[1] | 0x20) == 's' && (text[2] | 0x20) == 'b' &&
           (text[3] == ' ' || text[3] == '\0');
}

// `text` cut to fit a knode name: the word "USB" left out (it says nothing
// under /dev/usb and the names are short), spaces made '_' and anything odd
// dropped. "QEMU USB Keyboard" -> "QEMU_Keyboard"
static void usb_clean_name(const char* text, char* out, size_t size) {
    size_t n = 0;
    bool word_start = true;
    for (; *text != '\0' && n + 1 < size; text++) {
        if (word_start && usb_word_is_usb(text)) {
            text += 2;
            continue;
        }
        char c = *text;
        word_start = (c == ' ');
        if (c == ' ')
            c = '_';
        bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (keep && !(c == '_' && (n == 0 || out[n - 1] == '_')))
            out[n++] = c;
    }
    while (n > 0 && out[n - 1] == '_')
        n--;
    out[n] = '\0';
}

// A free name under `dir` for `dev`, into dev->node_name
static void usb_pick_name(void* dir, struct UsbDevice* dev) {
    char base[KNODE_NAME_LENGTH_MAX];
    usb_clean_name(dev->product, base, sizeof(base));
    if (base[0] == '\0')
        usb_clean_name(usb_kind_name(dev), base, sizeof(base));
    if (base[0] == '\0')
        strncpy(base, "device", sizeof(base));

    char* name = dev->node_name;
    strncpy(name, base, KNODE_NAME_LENGTH_MAX - 1);
    name[KNODE_NAME_LENGTH_MAX - 1] = '\0';

    for (uint32_t copy = 2; knode_find_by_name(dir, name) != NULL; copy++) {
        char suffix[12] = "-";
        utos(copy, &suffix[1]);
        size_t keep = KNODE_NAME_LENGTH_MAX - 1 - strlen(suffix);
        if (keep > strlen(base))
            keep = strlen(base);
        memcpy(name, base, keep);
        strncpy(&name[keep], suffix, KNODE_NAME_LENGTH_MAX - keep);
        name[KNODE_NAME_LENGTH_MAX - 1] = '\0';
    }
}

void usb_export_device(struct UsbDevice* dev) {
    void* dir = usb_get_directory();
    if (dir == NULL)
        return;

    usb_pick_name(dir, dev);
    void* node = create_knode(dev->node_name, dir);
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

    // "root" or the hub's /dev/usb name
    usb_property(node, "hub", dev->parent == NULL ? "root" : dev->parent->node_name);

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

// Free a property knode and the device value it names
static void usb_destroy_property(void* prop, void* parent) {
    for (uint32_t n = knode_get_reference_count(prop); n > 0; n--) {
        void* value = knode_get_reference(prop, n - 1);
        knode_remove_reference(prop, value);
        kfree(value);
    }
    destroy_knode(prop, parent);
}

void usb_export_all(void) {
    void* dir = usb_get_directory();
    if (dir == NULL)
        return;

    for (uint32_t n = knode_get_reference_count(dir); n > 0; n--) {
        void* node = knode_get_reference(dir, n - 1);
        for (uint32_t p = knode_get_reference_count(node); p > 0; p--)
            usb_destroy_property(knode_get_reference(node, p - 1), node);
        destroy_knode(node, dir);
    }

    usb_device_list_lock();
    for (uint32_t i = 0; i < usb_device_count(); i++)
        usb_export_device(usb_get_device(i));
    usb_device_list_unlock();
}
