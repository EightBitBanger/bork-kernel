#include <kernel/arch/x86/bus/usb/usb.h>
#include <kernel/arch/x86/bus/usb/usb_internal.h>
#include <kernel/scheduler/scheduler.h>

#include <kernel/malloc.h>
#include <kernel/util/string.h>

//
// Hot plug, and the "usb" thread
//
// One kernel thread does all USB work after boot:
//   - polls every keyboard and mouse (usb_hid.c), every few ms
//   - polls each hub's status change endpoint, which reports the ports
//     whose state changed
//   - checks the root hub ports of every controller for connect changes
//
// A connect change removes whatever was on that port (and everything
// behind it, when it was a hub) and enumerates what is there now, so a
// device pulled out and plugged back in comes back as a new device.
//

#define USB_HOTPLUG_POLL_MS     8       // Keyboards, mice, hub endpoints
#define USB_HOTPLUG_ROOT_MS     100     // Root hub ports
#define USB_HOTPLUG_DEBOUNCE_MS 100     // Connect debounce (USB 2.0 7.1.7.3)
#define USB_HOTPLUG_MAX_HUBS    8

struct UsbHubWatch {
    struct UsbDevice* hub;
    struct UsbIntPipe pipe;
};

// Written at boot before the thread starts, then by the thread only
static struct UsbHubWatch* hub_watches[USB_HOTPLUG_MAX_HUBS];
static uint32_t hub_watch_count = 0;

static bool hotplug_started = false;
static bool exports_stale = false;      // /dev/usb needs a rebuild

//
// Hubs
//

void usb_hotplug_watch_hub(struct UsbDevice* hub) {
    struct UsbController* hc = hub->hc;
    if (hc->ops->int_open == NULL || hc->ops->int_close == NULL)
        return;
    if (hub_watch_count >= USB_HOTPLUG_MAX_HUBS || hub->interfaces_known == 0)
        return;

    const struct UsbInterface* itf = &hub->interfaces[0];
    if (itf->int_in_endpoint == 0)
        return;

    struct UsbHubWatch* watch = (struct UsbHubWatch*)malloc(sizeof(struct UsbHubWatch));
    if (watch == NULL)
        return;
    memset(watch, 0, sizeof(*watch));
    watch->hub = hub;
    watch->pipe.dev        = hub;
    watch->pipe.endpoint   = itf->int_in_endpoint;
    watch->pipe.interval   = itf->int_in_interval;
    watch->pipe.max_packet = itf->int_in_max_packet;

    // One bit for the hub itself, then one per port
    uint16_t length = (uint16_t)((hub->hub_ports + 1 + 7) / 8);
    if (itf->int_in_max_packet != 0 && length > itf->int_in_max_packet)
        length = itf->int_in_max_packet;
    watch->pipe.length = length > USB_INT_MAX_LENGTH ? USB_INT_MAX_LENGTH : length;

    if (!hc->ops->int_open(hc, &watch->pipe)) {
        free(watch);
        return;
    }
    hub_watches[hub_watch_count++] = watch;
}

static void usb_hotplug_unwatch_hub(struct UsbDevice* hub) {
    for (uint32_t i = 0; i < hub_watch_count; i++) {
        struct UsbHubWatch* watch = hub_watches[i];
        if (watch->hub != hub)
            continue;

        hub->hc->ops->int_close(hub->hc, &watch->pipe);
        free(watch);
        for (; i + 1 < hub_watch_count; i++)
            hub_watches[i] = hub_watches[i + 1];
        hub_watch_count--;
        return;
    }
}

//
// Removing and adding devices (hc->lock held)
//

// The device on `port` of `parent` (NULL = root hub of hc), if any
static struct UsbDevice* usb_device_on_port(struct UsbController* hc, struct UsbDevice* parent, uint8_t port) {
    struct UsbDevice* found = NULL;
    usb_device_list_lock();
    for (uint32_t i = 0; i < usb_device_count() && found == NULL; i++) {
        struct UsbDevice* dev = usb_get_device(i);
        if (dev->hc == hc && dev->parent == parent && dev->port == port)
            found = dev;
    }
    usb_device_list_unlock();
    return found;
}

static struct UsbDevice* usb_first_child(struct UsbDevice* hub) {
    struct UsbDevice* found = NULL;
    usb_device_list_lock();
    for (uint32_t i = 0; i < usb_device_count() && found == NULL; i++) {
        struct UsbDevice* dev = usb_get_device(i);
        if (dev->parent == hub)
            found = dev;
    }
    usb_device_list_unlock();
    return found;
}

static void usb_remove_device(struct UsbDevice* dev) {
    struct UsbController* hc = dev->hc;

    // Everything behind a hub goes first
    struct UsbDevice* child;
    while ((child = usb_first_child(dev)) != NULL)
        usb_remove_device(child);

    usb_hid_detach(dev);
    usb_storage_detach(dev);
    if (dev->is_hub)
        usb_hotplug_unwatch_hub(dev);
    if (hc->ops->release_device != NULL)
        hc->ops->release_device(hc, dev);

    usb_unregister_device(dev);
    free(dev);
    exports_stale = true;
}

// The connection on a port changed: drop what was there, then enumerate
// what is there now
static void usb_port_changed(struct UsbController* hc, struct UsbDevice* parent, uint8_t port, bool connected) {
    struct UsbDevice* old = usb_device_on_port(hc, parent, port);
    if (old != NULL)
        usb_remove_device(old);

    if (!connected)
        return;

    usb_delay_ms(USB_HOTPLUG_DEBOUNCE_MS);
    uint8_t speed = parent == NULL ? hc->ops->port_reset(hc, port) : usb_hub_reset_port(parent, port);
    if (speed == 0)
        return;

    if (usb_enumerate_device(hc, parent, port, speed) == NULL) {
        if (parent != NULL)
            usb_hub_disable_port(parent, port);
    } else {
        exports_stale = true;
    }
}

//
// Polling
//

static void usb_hotplug_poll_roots(void) {
    for (uint32_t i = 0; i < usb_controller_count(); i++) {
        struct UsbController* hc = usb_get_controller(i);
        if (hc == NULL || hc->ops->port_changed == NULL)
            continue;

        mutex_lock(&hc->lock);
        for (uint8_t port = 1; port <= hc->port_count; port++) {
            bool connected;
            if (hc->ops->port_changed(hc, port, &connected))
                usb_port_changed(hc, NULL, port, connected);
        }
        mutex_unlock(&hc->lock);
    }
}

static void usb_hotplug_poll_hubs(void) {
    uint8_t bitmap[USB_INT_MAX_LENGTH];

    for (uint32_t i = 0; i < hub_watch_count; i++) {
        struct UsbHubWatch* watch = hub_watches[i];
        struct UsbDevice* hub = watch->hub;
        struct UsbController* hc = hub->hc;

        mutex_lock(&hc->lock);
        int length = hc->ops->int_poll(hc, &watch->pipe, bitmap);

        // A hub pulled out fails here until its own port change removes it
        bool handled = false;
        for (uint32_t port = 1; length > 0 && port <= hub->hub_ports && (int)(port / 8) < length; port++) {
            if (!(bitmap[port / 8] & (1U << (port % 8))))
                continue;

            struct UsbPortStatus status;
            if (!usb_hub_port_status_ack(hub, (uint8_t)port, &status))
                continue;
            if (status.change & USB_PORT_STAT_C_CONNECTION) {
                usb_port_changed(hc, hub, (uint8_t)port, (status.status & USB_PORT_STAT_CONNECTION) != 0);
                handled = true;
            }
        }
        mutex_unlock(&hc->lock);

        // The watch list may have changed under us: start over next time
        if (handled)
            return;
    }
}

static void usb_hotplug_thread(void* arg) {
    (void)arg;
    uint64_t last_root = 0;

    while (1) {
        usb_hid_poll();
        usb_hotplug_poll_hubs();

        if (usb_time_ms() - last_root >= USB_HOTPLUG_ROOT_MS) {
            last_root = usb_time_ms();
            usb_hotplug_poll_roots();
        }

        usb_storage_service();

        if (exports_stale) {
            exports_stale = false;
            usb_export_all();
        }

        thread_sleep(USB_HOTPLUG_POLL_MS);
    }
}

void usb_hotplug_start(void) {
    if (hotplug_started)
        return;
    hotplug_started = true;
    kthread_create(usb_hotplug_thread, NULL, PRIORITY_NORMAL, "usb");
}
