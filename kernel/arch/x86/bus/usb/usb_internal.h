#ifndef _USB_BUS_INTERNAL_H_
#define _USB_BUS_INTERNAL_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/arch/x86/bus/pci/pci.h>
#include <kernel/arch/x86/bus/usb/usb.h>
#include <kernel/mutex.h>

#define USB_MAX_CONTROLLERS     16

// What the core asks of a host controller driver. Every call runs with the
// controller's lock held, one at a time.
struct UsbHcdOps {
    const char* name;

    // Take the controller from the firmware, reset it and start it.
    // Sets hc->port_count. false drops the controller.
    bool (*start)(struct UsbController* hc);

    // Root hub port (from 1): if a device is connected, reset and enable the
    // port and return its USB_SPEED_*. 0 when the port is empty or will not
    // come up.
    uint8_t (*port_reset)(struct UsbController* hc, uint8_t port);

    // Move a freshly reset device (speed, parent, port, route and an initial
    // max_packet0 already filled in) from the default address to a unique
    // one, setting dev->address. USB 1/2 controllers use
    // usb_address_device_generic(); xHCI does it with a command.
    bool (*address_device)(struct UsbController* hc, struct UsbDevice* dev);

    // Optional: dev->max_packet0 changed after the first descriptor read
    bool (*update_max_packet0)(struct UsbController* hc, struct UsbDevice* dev);

    // Run one control transfer on endpoint 0. data is a kernel buffer of
    // setup->length bytes (none if 0). Returns the number of data stage
    // bytes moved, or a USB_ERR_* code.
    int (*control)(struct UsbController* hc, struct UsbDevice* dev,
                   const struct UsbSetupPacket* setup, void* data);

    // Optional: dev turned out to be a hub (is_hub, hub_ports and the TT
    // fields are filled in), before any of its ports are scanned
    void (*hub_configured)(struct UsbController* hc, struct UsbDevice* dev);

    // Optional: forget a device that failed enumeration
    void (*release_device)(struct UsbController* hc, struct UsbDevice* dev);
};

struct UsbController {
    const struct UsbHcdOps* ops;
    struct PCIDeviceInfo    pci;

    uint32_t index;             // Position in the started-controller list
    bool     started;
    uint8_t  port_count;        // Root hub ports
    uint8_t  next_address;      // usb_address_device_generic(): next free address

    mutex_t  lock;              // One transfer at a time
    void*    priv;              // Driver state
};

// Host controller drivers
extern const struct UsbHcdOps uhci_ops;
extern const struct UsbHcdOps xhci_ops;

// EHCI is not driven: it is taken from the BIOS, halted, and its ports are
// routed to the companion UHCI/OHCI controllers. Runs before those start.
void ehci_release_ports(const struct PCIDeviceInfo* pci);

// Enumeration (usb_enum.c)

// A device was found on `port` of `parent` (NULL = root hub) and that port
// has been reset. Address it, read it, configure it, scan it if it is a
// hub. Returns the device, or NULL if it did not enumerate.
struct UsbDevice* usb_enumerate_device(struct UsbController* hc, struct UsbDevice* parent,
                                       uint8_t port, uint8_t speed);

// SET_ADDRESS on the default address, for controllers that leave
// addressing to software (UHCI, OHCI, EHCI)
bool usb_address_device_generic(struct UsbController* hc, struct UsbDevice* dev);

// Hubs (usb_hub.c)

bool usb_hub_init(struct UsbDevice* hub);

// Device list (usb.c)

bool usb_register_device(struct UsbDevice* dev);

// Same as usb_control_transfer(), for callers that already hold hc->lock
int usb_control_locked(struct UsbDevice* dev, uint8_t request_type, uint8_t request,
                       uint16_t value, uint16_t index, void* data, uint16_t length);

// /dev/usb (usb_prop.c)

void usb_export_device(struct UsbDevice* dev);

// Helpers (usb.c)

// One zeroed, page-aligned page for the controller to DMA into, and its
// physical address. NULL when out of memory.
void* usb_dma_page(uint32_t* phys_out);
void  usb_dma_page_free(void* page);

// Busy-wait. Needs the PIT tick, so interrupts must be on.
void usb_delay_ms(uint32_t ms);
uint64_t usb_time_ms(void);

static inline void usb_mmio_barrier(void) {
    __asm__ volatile ("" ::: "memory");
}

#endif
