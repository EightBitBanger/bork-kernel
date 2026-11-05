#include <kernel/arch/x86/bus/usb/usb.h>
#include <kernel/arch/x86/bus/usb/usb_internal.h>
#include <kernel/driver/driver.h>
#include <kernel/driver/device.h>

#include <kernel/malloc.h>
#include <kernel/mutex.h>
#include <kernel/util/string.h>

//
// USB interfaces as driver model devices
//
// Enumeration and unplugging run with hc->lock held, but device_add and
// device_remove take the driver lock, which comes before every bus lock
// (driver.h). So both are queued here under the controller lock and done
// later by usb_bind_service() with no controller locked:
//
//   plugged in   usb_bind_attach      -> added[]   -> device_add per interface
//   pulled out   usb_bind_unplug      (gone, driver pipes closed on the host side)
//                usb_bind_retire      -> retired[] -> device_remove per interface,
//                                                     then the UsbDevice is freed
//
// A retired device is never freed before its driver's remove() has run, so
// a driver may look at its UsbDevice until then; its calls just fail.
//

struct UsbPipe {
    struct UsbBulkPipe    bulk;
    struct UsbController* hc;       // Controllers are never freed
    struct UsbDevice*     dev;      // NULL once the device is freed (a leaked pipe)
    bool                  open;     // Host side open; false once unplugged
    struct UsbPipe*       next;     // dev->pipes
};

static struct UsbDevice* added[USB_MAX_DEVICES];
static uint32_t added_count = 0;
static struct UsbDevice* retired[USB_MAX_DEVICES];
static uint32_t retired_count = 0;

// added[] / retired[]: taken under hc->lock, so it is the innermost lock
static mutex_t queue_lock;
// One usb_bind_service at a time; bound[] is only written under it
static mutex_t service_lock;
static bool locks_ready = false;

static void usb_bind_locks_init(void) {
    if (locks_ready)
        return;
    mutex_init(&queue_lock);
    mutex_init(&service_lock);
    locks_ready = true;
}

//
// Queues (hc->lock held)
//

void usb_bind_attach(struct UsbDevice* dev) {
    usb_bind_locks_init();
    if (dev->configuration == 0 || dev->is_hub)
        return;

    mutex_lock(&queue_lock);
    if (added_count < USB_MAX_DEVICES)
        added[added_count++] = dev;
    mutex_unlock(&queue_lock);
}

void usb_bind_unplug(struct UsbDevice* dev) {
    dev->gone = true;
    for (struct UsbPipe* pipe = dev->pipes; pipe != NULL; pipe = pipe->next) {
        if (pipe->open) {
            pipe->hc->ops->bulk_close(pipe->hc, &pipe->bulk);
            pipe->open = false;
        }
    }
}

void usb_bind_retire(struct UsbDevice* dev) {
    usb_bind_locks_init();
    mutex_lock(&queue_lock);

    // Not added yet: it never will be
    for (uint32_t i = 0; i < added_count; i++) {
        if (added[i] != dev)
            continue;
        for (; i + 1 < added_count; i++)
            added[i] = added[i + 1];
        added[--added_count] = NULL;
        break;
    }

    // Full only if the service has not run through USB_MAX_DEVICES unplugs:
    // then the device is leaked, which beats freeing it under a driver
    if (retired_count < USB_MAX_DEVICES)
        retired[retired_count++] = dev;
    mutex_unlock(&queue_lock);
}

//
// Service (no controller lock held)
//

static struct UsbDevice* usb_bind_pop(struct UsbDevice** queue, uint32_t* count) {
    struct UsbDevice* dev = NULL;
    mutex_lock(&queue_lock);
    if (*count > 0) {
        dev = queue[0];
        for (uint32_t i = 0; i + 1 < *count; i++)
            queue[i] = queue[i + 1];
        queue[--(*count)] = NULL;
    }
    mutex_unlock(&queue_lock);
    return dev;
}

static void usb_bind_add(struct UsbDevice* dev) {
    for (uint32_t i = 0; i < dev->interfaces_known; i++) {
        if (dev->claimed & (1U << i))
            continue;

        const struct UsbInterface* itf = &dev->interfaces[i];
        char name[DEVICE_NAME_MAX];
        snprintf(name, sizeof(name), "usb%u-%u:%u", (unsigned)usb_controller_index(dev->hc),
                 (unsigned)dev->address, (unsigned)itf->number);

        struct DeviceInfo info = {
            .bus        = BUS_USB,
            .vendor     = dev->descriptor.vendor_id,
            .product    = dev->descriptor.product_id,
            .class_code = itf->if_class,
            .subclass   = itf->if_subclass,
            .protocol   = itf->if_protocol,
            .bus_data   = dev,
            .bus_index  = itf->number,
        };
        dev->bound[i] = device_add(name, NULL, &info);
    }
}

static void usb_bind_remove(struct UsbDevice* dev) {
    for (uint32_t i = 0; i < USB_MAX_INTERFACES; i++) {
        if (dev->bound[i] != NULL) {
            device_remove(dev->bound[i]);
            dev->bound[i] = NULL;
        }
    }

    // A driver that left pipes open: they outlive the device, unusable
    // (usb_pipe_close still frees them). A thread inside a pipe call holds
    // the lock, so this also waits it out.
    mutex_lock(&dev->hc->lock);
    for (struct UsbPipe* pipe = dev->pipes; pipe != NULL; pipe = pipe->next)
        pipe->dev = NULL;
    dev->pipes = NULL;
    mutex_unlock(&dev->hc->lock);

    free(dev);
}

void usb_bind_service(void) {
    usb_bind_locks_init();
    mutex_lock(&service_lock);

    // Gone ones first: a device replugged on the same port is both
    struct UsbDevice* dev;
    while ((dev = usb_bind_pop(retired, &retired_count)) != NULL)
        usb_bind_remove(dev);
    while ((dev = usb_bind_pop(added, &added_count)) != NULL)
        usb_bind_add(dev);

    mutex_unlock(&service_lock);
}

//
// Driver calls
//

struct UsbDevice* usb_dev(const struct Device* dev) {
    if (dev == NULL || dev->bus != BUS_USB)
        return NULL;
    return (struct UsbDevice*)dev->bus_data;
}

const struct UsbInterface* usb_dev_interface(const struct Device* dev) {
    struct UsbDevice* udev = usb_dev(dev);
    if (udev == NULL)
        return NULL;
    for (uint32_t i = 0; i < udev->interfaces_known; i++) {
        if (udev->interfaces[i].number == dev->bus_index)
            return &udev->interfaces[i];
    }
    return NULL;
}

bool usb_dev_gone(const struct Device* dev) {
    struct UsbDevice* udev = usb_dev(dev);
    return udev == NULL || udev->gone;
}

int usb_dev_control(const struct Device* dev, uint8_t request_type, uint8_t request,
                    uint16_t value, uint16_t index, void* data, uint16_t length) {
    struct UsbDevice* udev = usb_dev(dev);
    if (udev == NULL)
        return USB_ERR_PARAM;

    // gone is set under this lock, so it can't change during the transfer
    mutex_lock(&udev->hc->lock);
    int result = udev->gone ? USB_ERR_GONE
                            : usb_control_locked(udev, request_type, request, value, index, data, length);
    mutex_unlock(&udev->hc->lock);
    return result;
}

struct UsbPipe* usb_pipe_open_bulk(const struct Device* dev, uint8_t endpoint, bool in) {
    struct UsbDevice* udev = usb_dev(dev);
    const struct UsbInterface* itf = usb_dev_interface(dev);
    if (udev == NULL || itf == NULL || endpoint == 0 || endpoint > 15)
        return NULL;

    // Only the interface's own endpoints, with the size its descriptor gave
    uint16_t max_packet;
    if (in && endpoint == itf->bulk_in_endpoint)
        max_packet = itf->bulk_in_max_packet;
    else if (!in && endpoint == itf->bulk_out_endpoint)
        max_packet = itf->bulk_out_max_packet;
    else
        return NULL;
    if (max_packet == 0)
        return NULL;

    struct UsbController* hc = udev->hc;
    if (hc->ops->bulk_open == NULL || hc->ops->bulk_transfer == NULL || hc->ops->bulk_close == NULL)
        return NULL;

    struct UsbPipe* pipe = (struct UsbPipe*)malloc(sizeof(struct UsbPipe));
    if (pipe == NULL)
        return NULL;
    memset(pipe, 0, sizeof(*pipe));
    pipe->hc  = hc;
    pipe->dev = udev;
    pipe->bulk.dev        = udev;
    pipe->bulk.endpoint   = endpoint;
    pipe->bulk.in         = in;
    pipe->bulk.max_packet = max_packet;

    mutex_lock(&hc->lock);
    bool ok = !udev->gone && hc->ops->bulk_open(hc, &pipe->bulk);
    if (ok) {
        pipe->open  = true;
        pipe->next  = udev->pipes;
        udev->pipes = pipe;
    }
    mutex_unlock(&hc->lock);

    if (!ok) {
        free(pipe);
        return NULL;
    }
    return pipe;
}

int usb_pipe_transfer(struct UsbPipe* pipe, void* data, uint32_t length, uint32_t timeout_ms) {
    if (pipe == NULL || length > USB_PIPE_MAX || (length != 0 && data == NULL))
        return USB_ERR_PARAM;

    mutex_lock(&pipe->hc->lock);
    int result = pipe->open ? pipe->hc->ops->bulk_transfer(pipe->hc, &pipe->bulk, data, length, timeout_ms)
                            : USB_ERR_GONE;
    mutex_unlock(&pipe->hc->lock);

    // Never more than was asked for, whatever the controller says
    if (result > (int)length)
        result = USB_ERR_IO;
    return result;
}

int usb_pipe_clear_halt(struct UsbPipe* pipe) {
    if (pipe == NULL)
        return USB_ERR_PARAM;

    mutex_lock(&pipe->hc->lock);
    int result = USB_ERR_GONE;
    if (pipe->open && pipe->dev != NULL) {
        uint16_t endpoint = (uint16_t)(pipe->bulk.endpoint | (pipe->bulk.in ? 0x80 : 0));
        result = usb_control_locked(pipe->dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT,
                                    USB_REQ_CLEAR_FEATURE, 0 /* ENDPOINT_HALT */, endpoint, NULL, 0);
        if (pipe->hc->ops->bulk_reset != NULL)
            pipe->hc->ops->bulk_reset(pipe->hc, &pipe->bulk);
    }
    mutex_unlock(&pipe->hc->lock);
    return result;
}

void usb_pipe_close(struct UsbPipe* pipe) {
    if (pipe == NULL)
        return;

    mutex_lock(&pipe->hc->lock);
    if (pipe->open) {
        pipe->hc->ops->bulk_close(pipe->hc, &pipe->bulk);
        pipe->open = false;
    }
    if (pipe->dev != NULL) {
        for (struct UsbPipe** link = &pipe->dev->pipes; *link != NULL; link = &(*link)->next) {
            if (*link == pipe) {
                *link = pipe->next;
                break;
            }
        }
    }
    mutex_unlock(&pipe->hc->lock);
    free(pipe);
}
