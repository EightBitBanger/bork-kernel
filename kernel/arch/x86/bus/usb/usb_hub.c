#include <kernel/arch/x86/bus/usb/usb.h>
#include <kernel/arch/x86/bus/usb/usb_internal.h>

#include <kernel/console/print.h>
#include <kernel/util/string.h>

#define USB_HUB_RESET_TIMEOUT_MS    500
#define USB_HUB_DEBOUNCE_MS         100     // Connect debounce (USB 2.0 7.1.7.3)
#define USB_HUB_RESET_RECOVERY_MS   10      // TRSTRCY

#define USB_HUB_PORT_REQ    (USB_TYPE_CLASS | USB_RECIP_OTHER)

// Hub ports are polled with GET_STATUS; the hub's interrupt endpoint is not
// used. Runs from usb_init() with hc->lock held.

static int usb_hub_port_feature(struct UsbDevice* hub, uint8_t request, uint16_t feature, uint8_t port) {
    return usb_control_locked(hub, USB_DIR_OUT | USB_HUB_PORT_REQ, request, feature, port, NULL, 0);
}

static bool usb_hub_port_status(struct UsbDevice* hub, uint8_t port, struct UsbPortStatus* out) {
    int length = usb_control_locked(hub, USB_DIR_IN | USB_HUB_PORT_REQ, USB_REQ_GET_STATUS,
                                    0, port, out, sizeof(*out));
    return length == (int)sizeof(*out);
}

// Reset one port and wait for it to come back enabled.
// Returns the attached device's USB_SPEED_*, or 0.
static uint8_t usb_hub_reset_port(struct UsbDevice* hub, uint8_t port) {
    struct UsbPortStatus st;

    if (usb_hub_port_feature(hub, USB_REQ_SET_FEATURE, USB_PORT_FEAT_RESET, port) < 0)
        return 0;

    uint64_t start = usb_time_ms();
    while (1) {
        usb_delay_ms(10);
        if (!usb_hub_port_status(hub, port, &st))
            return 0;
        if (!(st.status & USB_PORT_STAT_RESET))
            break;
        if (usb_time_ms() - start > USB_HUB_RESET_TIMEOUT_MS)
            return 0;
    }

    if (st.change & USB_PORT_STAT_C_RESET)
        usb_hub_port_feature(hub, USB_REQ_CLEAR_FEATURE, USB_PORT_FEAT_C_RESET, port);

    usb_delay_ms(USB_HUB_RESET_RECOVERY_MS);

    if (!usb_hub_port_status(hub, port, &st))
        return 0;
    if (!(st.status & USB_PORT_STAT_CONNECTION) || !(st.status & USB_PORT_STAT_ENABLE))
        return 0;

    if (st.status & USB_PORT_STAT_LOW_SPEED)  return USB_SPEED_LOW;
    if (st.status & USB_PORT_STAT_HIGH_SPEED) return USB_SPEED_HIGH;
    return USB_SPEED_FULL;
}

bool usb_hub_init(struct UsbDevice* hub) {
    if (hub->speed == USB_SPEED_SUPER) {
        // A USB 3 hub also shows up as a USB 2 hub on a USB 2 port, where
        // its low/full/high speed devices are found. Its SuperSpeed ports
        // need the USB 3 hub protocol, which is not implemented.
        print("USB: SuperSpeed hub ports not scanned\n");
        return false;
    }

    uint8_t buf[16];
    int length = usb_control_locked(hub, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE,
                                    USB_REQ_GET_DESCRIPTOR, USB_DESC_HUB << 8, 0, buf, sizeof(buf));
    if (length < 7 || buf[1] != USB_DESC_HUB) {
        print("USB: could not read a hub descriptor\n");
        return false;
    }

    const struct UsbHubDescriptor* desc = (const struct UsbHubDescriptor*)buf;
    hub->hub_ports      = desc->port_count;
    hub->hub_think_time = (uint8_t)((desc->characteristics >> 5) & 0x03);
    hub->hub_multi_tt   = hub->speed == USB_SPEED_HIGH && hub->descriptor.device_protocol == 2;

    if (hub->hc->ops->hub_configured != NULL)
        hub->hc->ops->hub_configured(hub->hc, hub);

    for (uint8_t port = 1; port <= hub->hub_ports; port++)
        usb_hub_port_feature(hub, USB_REQ_SET_FEATURE, USB_PORT_FEAT_POWER, port);

    usb_delay_ms((uint32_t)desc->power_on_to_good * 2 + USB_HUB_DEBOUNCE_MS);

    for (uint8_t port = 1; port <= hub->hub_ports; port++) {
        struct UsbPortStatus st;
        if (!usb_hub_port_status(hub, port, &st))
            continue;

        if (st.change & USB_PORT_STAT_C_CONNECTION)
            usb_hub_port_feature(hub, USB_REQ_CLEAR_FEATURE, USB_PORT_FEAT_C_CONNECTION, port);
        if (!(st.status & USB_PORT_STAT_CONNECTION))
            continue;

        uint8_t speed = usb_hub_reset_port(hub, port);
        if (speed == 0)
            continue;

        // A device that does not enumerate is left disabled, so it does not
        // keep answering on the default address
        if (usb_enumerate_device(hub->hc, hub, port, speed) == NULL)
            usb_hub_port_feature(hub, USB_REQ_CLEAR_FEATURE, USB_PORT_FEAT_ENABLE, port);
    }
    return true;
}
