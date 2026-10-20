#ifndef _USB_BUS_H_
#define _USB_BUS_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/arch/x86/bus/usb/usb_descriptors.h>

//
// USB bus
//
// Host controllers are found by the PCI scan (usb_pci_probe) and started by
// usb_init() once the scan is done. Every device on every root port, and
// behind USB 1.1 / 2.0 hubs, is then addressed, read and configured with its
// first configuration. Nothing claims the devices yet: this layer only
// enumerates them and offers control transfers to whatever driver comes next.
//
//   usb.c          controller list, usb_init(), control transfers
//   usb_enum.c     addressing a device and reading its descriptors
//   usb_hub.c      hub class: powering, resetting and scanning hub ports
//   usb_prop.c     the /dev/usb tree
//   hcd/uhci.c     USB 1.1 host controller (polled)
//   hcd/ehci.c     EHCI: takes it from the BIOS and routes its ports to the
//                  companion controllers (no high-speed support)
//   hcd/xhci.c     USB 3 host controller (polled), all speeds
//
// Interrupts are not used: transfers are polled to completion.
//

// Device speeds
#define USB_SPEED_LOW       1   // 1.5 Mbit/s
#define USB_SPEED_FULL      2   // 12 Mbit/s
#define USB_SPEED_HIGH      3   // 480 Mbit/s
#define USB_SPEED_SUPER     4   // 5 Gbit/s and up

#define USB_MAX_DEVICES     64
#define USB_MAX_HUB_DEPTH   5   // Hub tiers below the root hub (USB 2.0 11.1.2.1)
#define USB_STRING_MAX      32

struct UsbController;
struct PCIDeviceInfo;

struct UsbDevice {
    struct UsbController* hc;
    struct UsbDevice*     parent;       // Hub this device is plugged into, NULL on a root port

    uint8_t  port;                      // Port on the parent hub (or root hub), from 1
    uint8_t  root_port;                 // Root hub port the whole branch hangs off, from 1
    uint8_t  depth;                     // 0 on a root port, +1 per hub
    uint8_t  speed;                     // USB_SPEED_*
    uint8_t  address;                   // Assigned bus address (1-127)
    uint16_t max_packet0;               // Endpoint 0 max packet size
    uint32_t route;                     // xHCI route string (port per hub tier, 4 bits each)

    struct UsbDeviceDescriptor descriptor;
    uint8_t  configuration;             // bConfigurationValue set, 0 = unconfigured
    uint8_t  interface_count;

    // First interface of the active configuration. Composite devices often
    // say "class 0, see interfaces" in the device descriptor.
    uint8_t  if_class;
    uint8_t  if_subclass;
    uint8_t  if_protocol;

    char manufacturer[USB_STRING_MAX];
    char product[USB_STRING_MAX];
    char serial[USB_STRING_MAX];

    // Hub class
    bool     is_hub;
    uint8_t  hub_ports;
    bool     hub_multi_tt;
    uint8_t  hub_think_time;            // TT think time, 0-3 (x 8 FS bit times)

    void*    hcd_data;                  // Owned by the host controller driver
};

//
// Setup from the PCI scan
//

// PCI class driver for every USB host controller (class 0x0C, subclass 0x03).
// Only records the controller; usb_init() starts it.
struct PCIScanContext;
void usb_pci_probe(const struct PCIDeviceInfo* info, struct PCIScanContext* ctx);

// Start every recorded controller and enumerate what is plugged in.
// Controllers already started (a second pci_init()) are left alone.
void usb_init(void);

//
// Devices
//

uint32_t usb_device_count(void);
struct UsbDevice* usb_get_device(uint32_t index);

// Number of host controllers started
uint32_t usb_controller_count(void);
// "uhci" / "xhci" / ..., and the controller's position in usb_controller_count()
const char* usb_controller_name(const struct UsbController* hc);
uint32_t usb_controller_index(const struct UsbController* hc);

// Issue a control transfer on endpoint 0. `data` may be any kernel buffer up
// to USB_CONTROL_MAX bytes. Returns the number of bytes in the data stage,
// or a negative USB_ERR_* code.
#define USB_CONTROL_MAX     4096

#define USB_ERR_TIMEOUT     (-1)
#define USB_ERR_STALL       (-2)
#define USB_ERR_IO          (-3)    // CRC, babble, bit stuffing, ...
#define USB_ERR_PARAM       (-4)
#define USB_ERR_NOMEM       (-5)

int usb_control_transfer(struct UsbDevice* dev, uint8_t request_type, uint8_t request,
                         uint16_t value, uint16_t index, void* data, uint16_t length);

// Standard requests built on usb_control_transfer()
int usb_get_descriptor(struct UsbDevice* dev, uint8_t type, uint8_t index, uint16_t lang, void* buf, uint16_t length);
int usb_set_configuration(struct UsbDevice* dev, uint8_t value);

// Names for printing
const char* usb_speed_name(uint8_t speed);
const char* usb_class_name(uint8_t class_code);

#endif
