#include <kernel/arch/x86/bus/usb/usb.h>
#include <kernel/arch/x86/bus/usb/usb_internal.h>

#include <kernel/malloc.h>
#include <kernel/console/print.h>
#include <kernel/util/string.h>

#define USB_CONFIG_BUFFER       1024    // Longest configuration descriptor set read
#define USB_ENUM_RETRIES        3

// Everything here runs from usb_init() with hc->lock held

static int usb_get_descriptor_locked(struct UsbDevice* dev, uint8_t type, uint8_t index, uint16_t lang,
                                     void* buf, uint16_t length) {
    return usb_control_locked(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                              USB_REQ_GET_DESCRIPTOR, (uint16_t)((type << 8) | index), lang, buf, length);
}

bool usb_address_device_generic(struct UsbController* hc, struct UsbDevice* dev) {
    if (hc->next_address == 0 || hc->next_address > 127)
        return false;   // Out of addresses on this bus

    uint8_t address = hc->next_address;
    dev->address = 0;
    int result = usb_control_locked(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                                    USB_REQ_SET_ADDRESS, address, 0, NULL, 0);
    if (result < 0)
        return false;

    usb_delay_ms(10);   // SET_ADDRESS recovery (2 ms in USB 2.0 9.2.6.3; some devices need more)
    dev->address = address;
    hc->next_address++;
    return true;
}

// String descriptor `index` as printable ASCII (anything else becomes '?').
// Leaves `out` empty if the device has no such string.
static void usb_read_string(struct UsbDevice* dev, uint8_t index, uint16_t lang, char* out, uint32_t out_size) {
    out[0] = '\0';
    if (index == 0)
        return;

    uint8_t buf[256];
    int length = usb_get_descriptor_locked(dev, USB_DESC_STRING, index, lang, buf, 255);
    if (length < 2 || buf[1] != USB_DESC_STRING)
        return;
    if (buf[0] < length)
        length = buf[0];

    uint32_t n = 0;
    for (int i = 2; i + 1 < length && n + 1 < out_size; i += 2) {
        uint16_t c = (uint16_t)(buf[i] | (buf[i + 1] << 8));
        out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
}

static void usb_read_strings(struct UsbDevice* dev) {
    const struct UsbDeviceDescriptor* d = &dev->descriptor;
    if (d->manufacturer_index == 0 && d->product_index == 0 && d->serial_index == 0)
        return;

    // String 0 lists the supported languages; take the first one
    uint8_t langs[8];
    uint16_t lang = 0x0409;     // English (US)
    int length = usb_get_descriptor_locked(dev, USB_DESC_STRING, 0, 0, langs, sizeof(langs));
    if (length >= 4 && langs[1] == USB_DESC_STRING)
        lang = (uint16_t)(langs[2] | (langs[3] << 8));
    else if (length < 0)
        return;     // No strings at all

    usb_read_string(dev, d->manufacturer_index, lang, dev->manufacturer, sizeof(dev->manufacturer));
    usb_read_string(dev, d->product_index, lang, dev->product, sizeof(dev->product));
    usb_read_string(dev, d->serial_index, lang, dev->serial, sizeof(dev->serial));
}

// Read configuration 0, note its first interface and select it
static bool usb_configure(struct UsbDevice* dev) {
    struct UsbConfigDescriptor header;
    int length = usb_get_descriptor_locked(dev, USB_DESC_CONFIGURATION, 0, 0, &header, sizeof(header));
    if (length < (int)sizeof(header) || header.type != USB_DESC_CONFIGURATION)
        return false;

    uint16_t total = header.total_length;
    if (total < sizeof(header))
        return false;
    if (total > USB_CONFIG_BUFFER)
        total = USB_CONFIG_BUFFER;

    uint8_t* buf = (uint8_t*)malloc(total);
    if (buf == NULL)
        return false;

    length = usb_get_descriptor_locked(dev, USB_DESC_CONFIGURATION, 0, 0, buf, total);
    if (length < (int)sizeof(header)) {
        free(buf);
        return false;
    }

    // Walk the descriptors after the configuration header. Only alternate
    // setting 0 of each interface counts.
    bool first = true;
    struct UsbInterface* current = NULL;    // Interface the endpoints below belong to
    for (int offset = buf[0]; offset + 2 <= length; ) {
        uint8_t desc_length = buf[offset];
        uint8_t desc_type   = buf[offset + 1];
        if (desc_length < 2 || offset + desc_length > length)
            break;

        if (desc_type == USB_DESC_INTERFACE && desc_length >= sizeof(struct UsbInterfaceDescriptor)) {
            const struct UsbInterfaceDescriptor* itf = (const struct UsbInterfaceDescriptor*)&buf[offset];
            current = NULL;
            if (itf->alternate_setting == 0) {
                if (dev->interfaces_known < USB_MAX_INTERFACES) {
                    current = &dev->interfaces[dev->interfaces_known++];
                    current->number      = itf->interface_number;
                    current->if_class    = itf->interface_class;
                    current->if_subclass = itf->interface_subclass;
                    current->if_protocol = itf->interface_protocol;
                }
                dev->interface_count++;
                if (first) {
                    dev->if_class    = itf->interface_class;
                    dev->if_subclass = itf->interface_subclass;
                    dev->if_protocol = itf->interface_protocol;
                    first = false;
                }
            }
        } else if (desc_type == USB_DESC_ENDPOINT && desc_length >= sizeof(struct UsbEndpointDescriptor) &&
                   current != NULL) {
            const struct UsbEndpointDescriptor* ep = (const struct UsbEndpointDescriptor*)&buf[offset];
            uint8_t  number = ep->address & 0x0F;
            bool     in     = (ep->address & 0x80) != 0;
            uint8_t  type   = ep->attributes & 0x03;
            uint16_t mps    = ep->max_packet & 0x07FF;
            if (number != 0 && type == 0x03 && in && current->int_in_endpoint == 0) {
                current->int_in_endpoint   = number;
                current->int_in_interval   = ep->interval;
                current->int_in_max_packet = mps;
            } else if (number != 0 && type == 0x02 && in && current->bulk_in_endpoint == 0) {
                current->bulk_in_endpoint   = number;
                current->bulk_in_max_packet = mps;
            } else if (number != 0 && type == 0x02 && !in && current->bulk_out_endpoint == 0) {
                current->bulk_out_endpoint   = number;
                current->bulk_out_max_packet = mps;
            }
        }
        offset += desc_length;
    }
    free(buf);

    if (usb_control_locked(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                           USB_REQ_SET_CONFIGURATION, header.configuration_value, 0, NULL, 0) < 0)
        return false;

    dev->configuration = header.configuration_value;
    return true;
}

static void usb_print_error(const char* what, int error) {
    usb_print("USB: ");
    usb_print(what);
    usb_print(" (");
    switch (error) {
        case USB_ERR_TIMEOUT: usb_print("no response"); break;
        case USB_ERR_STALL:   usb_print("stalled"); break;
        case USB_ERR_IO:      usb_print("transfer error"); break;
        default:              usb_print_int(error); break;
    }
    usb_print(")\n");
}

// Reset the port a device hangs off again
static uint8_t usb_reset_port_again(struct UsbController* hc, struct UsbDevice* parent, uint8_t port) {
    return parent == NULL ? hc->ops->port_reset(hc, port) : usb_hub_reset_port(parent, port);
}

// First look at a device still on the default address, done the way
// Windows and Linux do it: ask for 64 bytes of the device descriptor with a
// 64-byte max packet, which every device answers with at least its first 8
// bytes, then reset the port again. Many flash drives expect exactly this
// and fail a first read made after SET_ADDRESS.
//
// Sets dev->max_packet0 from the answer. Returns 0, or USB_ERR_TIMEOUT
// when nothing answers at all (an empty port that reads as connected), or
// another USB_ERR_* when the port did not come back from the reset.
static int usb_first_look(struct UsbController* hc, struct UsbDevice* dev) {
    uint8_t buf[64];
    int length = USB_ERR_IO;
    bool answered = false;

    dev->address = 0;
    if (dev->speed != USB_SPEED_LOW)
        dev->max_packet0 = 64;      // Low speed is always 8

    for (int attempt = 0; attempt < USB_ENUM_RETRIES && length < 8; attempt++) {
        if (attempt > 0) usb_delay_ms(10);
        length = usb_get_descriptor_locked(dev, USB_DESC_DEVICE, 0, 0, buf, sizeof(buf));
        if (length != USB_ERR_TIMEOUT)
            answered = true;
    }
    if (!answered)
        return USB_ERR_TIMEOUT;

    if (length >= 8 && buf[1] == USB_DESC_DEVICE) {
        uint8_t mps = buf[7];
        if (mps == 8 || mps == 16 || mps == 32 || mps == 64)
            dev->max_packet0 = mps;
    } else {
        dev->max_packet0 = 8;       // Unknown: 8 always works
    }

    uint8_t speed = usb_reset_port_again(hc, dev->parent, dev->port);
    if (speed == 0)
        return USB_ERR_IO;
    dev->speed = speed;
    return 0;
}

static void usb_print_device(const struct UsbDevice* dev) {
    uint8_t cls = dev->descriptor.device_class ? dev->descriptor.device_class : dev->if_class;

    usb_print("USB: ");
    usb_print_hex16(dev->descriptor.vendor_id);
    usb_print(":");
    usb_print_hex16(dev->descriptor.product_id);
    usb_print(" ");
    usb_print(usb_class_name(cls));
    usb_print(" (");
    usb_print(usb_speed_name(dev->speed));
    usb_print(")");
    if (dev->product[0] != '\0') {
        usb_print(" ");
        usb_print(dev->product);
    }
    usb_print("\n");
}

static void usb_discard_device(struct UsbController* hc, struct UsbDevice* dev) {
    if (hc->ops->release_device != NULL)
        hc->ops->release_device(hc, dev);
    free(dev);
}

static uint16_t usb_initial_max_packet0(uint8_t speed) {
    switch (speed) {
        case USB_SPEED_HIGH:  return 64;
        case USB_SPEED_SUPER: return 512;
        default:              return 8;     // Low and full speed: 8 always works
    }
}

struct UsbDevice* usb_enumerate_device(struct UsbController* hc, struct UsbDevice* parent,
                                       uint8_t port, uint8_t speed) {
    if (parent != NULL && parent->depth + 1 > USB_MAX_HUB_DEPTH) {
        usb_print("USB: hubs nested too deep, device ignored\n");
        return NULL;
    }
    if (usb_device_count() >= USB_MAX_DEVICES)
        return NULL;

    struct UsbDevice* dev = (struct UsbDevice*)malloc(sizeof(struct UsbDevice));
    if (dev == NULL)
        return NULL;
    memset(dev, 0, sizeof(*dev));

    dev->hc     = hc;
    dev->parent = parent;
    dev->port   = port;
    dev->speed  = speed;
    if (parent == NULL) {
        dev->root_port = port;
        dev->depth     = 0;
        dev->route     = 0;
    } else {
        // Route string: one nibble per hub tier, ports above 15 saturate
        dev->root_port = parent->root_port;
        dev->depth     = (uint8_t)(parent->depth + 1);
        dev->route     = parent->route | ((uint32_t)(port > 15 ? 15 : port) << (4 * parent->depth));
    }
    dev->max_packet0 = usb_initial_max_packet0(speed);

    // Controllers that leave addressing to software get a look at the
    // device on the default address first. xHCI addresses it itself.
    if (hc->ops->address_device == usb_address_device_generic) {
        int result = usb_first_look(hc, dev);
        if (result != 0) {
            // Nothing answering is an empty port, not worth a message
            if (result != USB_ERR_TIMEOUT)
                usb_print("USB: port did not come back after a reset\n");
            usb_discard_device(hc, dev);
            return NULL;
        }
    }

    if (!hc->ops->address_device(hc, dev)) {
        usb_discard_device(hc, dev);
        return NULL;
    }

    // The first 8 bytes carry bMaxPacketSize0, which every later transfer
    // depends on
    int length = USB_ERR_IO;
    for (int attempt = 0; attempt < USB_ENUM_RETRIES && length < 8; attempt++) {
        if (attempt > 0) usb_delay_ms(10);
        length = usb_get_descriptor_locked(dev, USB_DESC_DEVICE, 0, 0, &dev->descriptor, 8);
    }
    if (length < 8) {
        usb_print_error("could not read a device descriptor", length);
        usb_discard_device(hc, dev);
        return NULL;
    }

    uint16_t max_packet0 = dev->descriptor.max_packet0;
    if (speed == USB_SPEED_SUPER && dev->descriptor.usb_version >= 0x0300 && max_packet0 < 16)
        max_packet0 = (uint16_t)(1U << max_packet0);    // USB 3 encodes it as 2^n
    if (max_packet0 >= 8 && max_packet0 != dev->max_packet0) {
        dev->max_packet0 = max_packet0;
        if (hc->ops->update_max_packet0 != NULL && !hc->ops->update_max_packet0(hc, dev)) {
            usb_discard_device(hc, dev);
            return NULL;
        }
    }

    length = usb_get_descriptor_locked(dev, USB_DESC_DEVICE, 0, 0, &dev->descriptor, sizeof(dev->descriptor));
    if (length < (int)sizeof(dev->descriptor)) {
        usb_print_error("could not read the full device descriptor", length);
        usb_discard_device(hc, dev);
        return NULL;
    }

    usb_read_strings(dev);
    if (!usb_configure(dev))
        usb_print("USB: could not configure a device, left unconfigured\n");

    dev->is_hub = dev->descriptor.device_class == USB_CLASS_HUB ||
                  (dev->descriptor.device_class == 0 && dev->if_class == USB_CLASS_HUB);

    if (!usb_register_device(dev)) {
        usb_discard_device(hc, dev);
        return NULL;
    }
    usb_print_device(dev);

    if (dev->configuration != 0) {
        usb_hid_attach(dev);
        usb_storage_attach(dev);
    }

    // Children come after their hub in the device list
    if (dev->is_hub && dev->configuration != 0)
        usb_hub_init(dev);

    return dev;
}
