#include <kernel/arch/x86/bus/usb/usb.h>
#include <kernel/arch/x86/bus/usb/usb_internal.h>
#include <kernel/arch/x86/bus/pci/pci.h>
#include <kernel/arch/x86/virtual/vmm.h>

#include <kernel/console/print.h>
#include <kernel/util/string.h>
#include <kernel/util/timer.h>

static struct UsbController controllers[USB_MAX_CONTROLLERS];
static uint32_t controller_slots = 0;       // Entries of controllers[] in use
static uint32_t controllers_started = 0;

static struct UsbDevice* devices[USB_MAX_DEVICES];
static uint32_t device_total = 0;
static mutex_t device_list_lock;            // devices[] against lsusb while hot plugging
static bool device_list_ready = false;

// Messages go to the boot console only. Once the desktop runs, text printed
// to the screen would land on top of it.
static bool usb_quiet = false;

void usb_print(const char* text) {
    if (!usb_quiet) print(text);
}

void usb_print_hex16(uint16_t value) {
    if (!usb_quiet) print_hex16(value);
}

void usb_print_int(int32_t value) {
    if (!usb_quiet) print_int(value);
}

bool usb_is_quiet(void) {
    return usb_quiet;
}

//
// Helpers
//

void* usb_dma_page(uint32_t* phys_out) {
    void* page = vmm_alloc_pages(1);
    if (page == NULL) return NULL;

    memset(page, 0, PAGE_SIZE);
    *phys_out = vmm_get_phys_addr(page);
    if (*phys_out == 0) {
        vmm_free_pages(page, 1);
        return NULL;
    }
    return page;
}

void usb_dma_page_free(void* page) {
    if (page != NULL)
        vmm_free_pages(page, 1);
}

uint64_t usb_time_ms(void) {
    return timer_get_ms();
}

void usb_delay_ms(uint32_t ms) {
    uint64_t start = timer_get_ms();
    while (timer_get_ms() - start < ms)
        __asm__ volatile ("pause");
}

const char* usb_speed_name(uint8_t speed) {
    switch (speed) {
        case USB_SPEED_LOW:   return "low";
        case USB_SPEED_FULL:  return "full";
        case USB_SPEED_HIGH:  return "high";
        case USB_SPEED_SUPER: return "super";
        default:              return "?";
    }
}

const char* usb_class_name(uint8_t class_code) {
    switch (class_code) {
        case USB_CLASS_PER_INTERFACE: return "composite";
        case USB_CLASS_AUDIO:         return "audio";
        case USB_CLASS_COMM:          return "comm";
        case USB_CLASS_HID:           return "hid";
        case USB_CLASS_PRINTER:       return "printer";
        case USB_CLASS_MASS_STORAGE:  return "storage";
        case USB_CLASS_HUB:           return "hub";
        case USB_CLASS_VIDEO:         return "video";
        case USB_CLASS_WIRELESS:      return "wireless";
        case USB_CLASS_MISC:          return "misc";
        case USB_CLASS_VENDOR:        return "vendor";
        default:                      return "other";
    }
}

//
// Controllers
//

void usb_pci_probe(const struct PCIDeviceInfo* info, struct PCIScanContext* ctx) {
    (void)ctx;

    // pci_init() may run again; keep one entry per function
    for (uint32_t i = 0; i < controller_slots; i++) {
        const struct PCIDeviceInfo* known = &controllers[i].pci;
        if (known->bus == info->bus && known->dev == info->dev && known->func == info->func)
            return;
    }

    const struct UsbHcdOps* ops = NULL;
    switch (info->prog_if) {
        case PCI_USB_UHCI: ops = &uhci_ops; break;
        case PCI_USB_OHCI: ops = &ohci_ops; break;
        case PCI_USB_XHCI: ops = &xhci_ops; break;
        case PCI_USB_EHCI: break;   // Handled in usb_init()
        default:
            return;     // USB device side, or not a host controller
    }

    if (controller_slots >= USB_MAX_CONTROLLERS) {
        usb_print("USB: too many host controllers, one skipped\n");
        return;
    }

    struct UsbController* hc = &controllers[controller_slots++];
    memset(hc, 0, sizeof(*hc));
    hc->ops = ops;
    hc->pci = *info;
    hc->next_address = 1;
    mutex_init(&hc->lock);
}

uint32_t usb_controller_count(void) {
    return controllers_started;
}

const char* usb_controller_name(const struct UsbController* hc) {
    if (hc == NULL) return "?";
    return hc->ops != NULL ? hc->ops->name : "ehci";
}

uint32_t usb_controller_index(const struct UsbController* hc) {
    return hc != NULL ? hc->index : 0;
}

static void usb_start_controller(struct UsbController* hc) {
    mutex_lock(&hc->lock);

    if (!hc->ops->start(hc)) {
        mutex_unlock(&hc->lock);
        usb_print("USB: ");
        usb_print(hc->ops->name);
        usb_print(" controller did not start\n");
        return;
    }
    hc->started = true;
    hc->index = controllers_started++;

    for (uint8_t port = 1; port <= hc->port_count; port++) {
        uint8_t speed = hc->ops->port_reset(hc, port);
        if (speed != 0)
            usb_enumerate_device(hc, NULL, port, speed);
    }

    // Start hot plug tracking from here: connect changes so far are the
    // devices just enumerated
    for (uint8_t port = 1; port <= hc->port_count; port++) {
        bool connected;
        hc->ops->port_changed(hc, port, &connected);
    }

    mutex_unlock(&hc->lock);
}

void usb_init(void) {
    if (!device_list_ready) {
        mutex_init(&device_list_lock);
        device_list_ready = true;
    }

    // A blank line between the storage messages of the PCI scan and the
    // USB ones, when there is something new to start
    for (uint32_t i = 0; i < controller_slots; i++) {
        if (!controllers[i].started) {
            usb_print("\n");
            break;
        }
    }

    // EHCI first: until its ports are routed to the companion controllers,
    // high-speed capable ports are invisible to them
    for (uint32_t i = 0; i < controller_slots; i++) {
        struct UsbController* hc = &controllers[i];
        if (hc->ops == NULL && !hc->started) {
            ehci_release_ports(&hc->pci);
            hc->started = true;
        }
    }

    for (uint32_t i = 0; i < controller_slots; i++) {
        struct UsbController* hc = &controllers[i];
        if (hc->ops != NULL && !hc->started)
            usb_start_controller(hc);
    }

    // Disks found above get their /mnt entries (with no controller locked)
    usb_storage_service();

    // From here on devices come and go at run time: quietly
    if (controllers_started > 0) {
        usb_quiet = true;
        usb_hotplug_start();
    }
}

struct UsbController* usb_get_controller(uint32_t index) {
    for (uint32_t i = 0; i < controller_slots; i++) {
        struct UsbController* hc = &controllers[i];
        if (hc->ops != NULL && hc->started && hc->index == index)
            return hc;
    }
    return NULL;
}

//
// Devices
//

bool usb_register_device(struct UsbDevice* dev) {
    usb_device_list_lock();
    if (device_total >= USB_MAX_DEVICES) {
        usb_device_list_unlock();
        return false;
    }
    devices[device_total++] = dev;
    usb_device_list_unlock();

    usb_export_device(dev);
    return true;
}

void usb_unregister_device(struct UsbDevice* dev) {
    usb_device_list_lock();
    for (uint32_t i = 0; i < device_total; i++) {
        if (devices[i] != dev)
            continue;
        for (; i + 1 < device_total; i++)
            devices[i] = devices[i + 1];
        devices[--device_total] = NULL;
        break;
    }
    usb_device_list_unlock();
}

void usb_device_list_lock(void) {
    if (device_list_ready) mutex_lock(&device_list_lock);
}

void usb_device_list_unlock(void) {
    if (device_list_ready) mutex_unlock(&device_list_lock);
}

uint32_t usb_device_count(void) {
    return device_total;
}

struct UsbDevice* usb_get_device(uint32_t index) {
    return index < device_total ? devices[index] : NULL;
}

//
// Control transfers
//

int usb_control_locked(struct UsbDevice* dev, uint8_t request_type, uint8_t request,
                       uint16_t value, uint16_t index, void* data, uint16_t length) {
    if (dev == NULL || dev->hc == NULL || length > USB_CONTROL_MAX || (length != 0 && data == NULL))
        return USB_ERR_PARAM;

    struct UsbSetupPacket setup = { request_type, request, value, index, length };
    return dev->hc->ops->control(dev->hc, dev, &setup, data);
}

int usb_control_transfer(struct UsbDevice* dev, uint8_t request_type, uint8_t request,
                         uint16_t value, uint16_t index, void* data, uint16_t length) {
    if (dev == NULL || dev->hc == NULL)
        return USB_ERR_PARAM;

    mutex_lock(&dev->hc->lock);
    int result = usb_control_locked(dev, request_type, request, value, index, data, length);
    mutex_unlock(&dev->hc->lock);
    return result;
}

int usb_get_descriptor(struct UsbDevice* dev, uint8_t type, uint8_t index, uint16_t lang, void* buf, uint16_t length) {
    return usb_control_transfer(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                                USB_REQ_GET_DESCRIPTOR, (uint16_t)((type << 8) | index), lang, buf, length);
}

int usb_set_configuration(struct UsbDevice* dev, uint8_t value) {
    return usb_control_transfer(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                                USB_REQ_SET_CONFIGURATION, value, 0, NULL, 0);
}
