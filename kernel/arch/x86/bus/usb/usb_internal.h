#ifndef _USB_BUS_INTERNAL_H_
#define _USB_BUS_INTERNAL_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/arch/x86/bus/pci/pci.h>
#include <kernel/arch/x86/bus/usb/usb.h>
#include <kernel/mutex.h>

#define USB_MAX_CONTROLLERS     16

struct UsbIntPipe;
struct UsbBulkPipe;

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

    // Root hub port (from 1): true if its connection changed since the
    // last call (the change is acknowledged); *connected gets the state now
    bool (*port_changed)(struct UsbController* hc, uint8_t port, bool* connected);

    // Optional: forget a device that failed enumeration or was unplugged
    void (*release_device)(struct UsbController* hc, struct UsbDevice* dev);

    // Interrupt IN endpoint: open queues the first transfer. poll never
    // waits: it returns the bytes of a completed transfer (copied to buf,
    // at most pipe->length) and queues the next one, 0 when nothing came
    // yet, or a USB_ERR_* code (the transfer is queued again regardless).
    bool (*int_open)(struct UsbController* hc, struct UsbIntPipe* pipe);
    int  (*int_poll)(struct UsbController* hc, struct UsbIntPipe* pipe, void* buf);

    // Take the endpoint off the schedule and free what int_open allocated
    void (*int_close)(struct UsbController* hc, struct UsbIntPipe* pipe);

    // Bulk endpoints. open prepares the pipe; transfer moves up to
    // USB_BULK_MAX bytes and waits for it (timeout_ms), returning the bytes
    // moved or a USB_ERR_* code; reset clears the host side after a stall
    // (data toggle back to DATA0, a halted endpoint restarted), to go with
    // CLEAR_FEATURE(ENDPOINT_HALT) on the device; close frees the pipe.
    bool (*bulk_open)(struct UsbController* hc, struct UsbBulkPipe* pipe);
    int  (*bulk_transfer)(struct UsbController* hc, struct UsbBulkPipe* pipe, void* data,
                          uint32_t length, uint32_t timeout_ms);
    void (*bulk_reset)(struct UsbController* hc, struct UsbBulkPipe* pipe);
    void (*bulk_close)(struct UsbController* hc, struct UsbBulkPipe* pipe);
};

#define USB_BULK_MAX            4096    // One page of bounce buffer

struct UsbBulkPipe {
    struct UsbDevice* dev;
    uint8_t  endpoint;          // Endpoint number (1-15)
    bool     in;
    uint16_t max_packet;
    uint8_t  toggle;            // Next data toggle (drivers that track it in software)
    void*    hcd;               // Owned by the host controller driver
};

#define USB_INT_MAX_LENGTH      64

struct UsbIntPipe {
    struct UsbDevice* dev;
    uint8_t  endpoint;          // Endpoint number (1-15), IN
    uint8_t  interval;          // bInterval
    uint16_t max_packet;
    uint16_t length;            // Bytes asked per transfer (<= USB_INT_MAX_LENGTH)
    void*    hcd;               // Owned by the host controller driver
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
extern const struct UsbHcdOps ohci_ops;
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

// Reset one hub port and wait for it to come back enabled.
// Returns the attached device's USB_SPEED_*, or 0.
uint8_t usb_hub_reset_port(struct UsbDevice* hub, uint8_t port);

// GET_STATUS of a hub port, acknowledging its change bits
bool usb_hub_port_status_ack(struct UsbDevice* hub, uint8_t port, struct UsbPortStatus* out);
void usb_hub_disable_port(struct UsbDevice* hub, uint8_t port);

// HID keyboards and mice (usb_hid.c)

// A device just enumerated: claim its boot keyboard / mouse interfaces
void usb_hid_attach(struct UsbDevice* dev);
// Poll every keyboard and mouse once ("usb" thread)
void usb_hid_poll(void);
// The device is going away: close its endpoints
void usb_hid_detach(struct UsbDevice* dev);

// Mass storage (usb_storage.c)

// A device just enumerated: claim a bulk-only SCSI interface and register
// it as a block device (hc->lock held). Mounting waits for usb_storage_service.
void usb_storage_attach(struct UsbDevice* dev);
// The device is going away (hc->lock held): its I/O fails from now on, and
// its block device and mounts go at the next usb_storage_service
void usb_storage_detach(struct UsbDevice* dev);
// Mount disks attached and remove disks detached since the last call. No
// controller lock may be held: mounting reads the disk.
void usb_storage_service(void);

// Hot plug (usb_hotplug.c)

// Start the "usb" thread (once)
void usb_hotplug_start(void);
// Watch a configured hub's status change endpoint (hc->lock held)
void usb_hotplug_watch_hub(struct UsbDevice* hub);

// Started controller number `index` (usb_controller_index), or NULL
struct UsbController* usb_get_controller(uint32_t index);

// Boot console only: silent once usb_init() is done
void usb_print(const char* text);
void usb_print_hex16(uint16_t value);
void usb_print_int(int32_t value);
bool usb_is_quiet(void);

// Device list (usb.c)

bool usb_register_device(struct UsbDevice* dev);
void usb_unregister_device(struct UsbDevice* dev);

// Same as usb_control_transfer(), for callers that already hold hc->lock
int usb_control_locked(struct UsbDevice* dev, uint8_t request_type, uint8_t request,
                       uint16_t value, uint16_t index, void* data, uint16_t length);

// /dev/usb (usb_prop.c)

void usb_export_device(struct UsbDevice* dev);
// Rebuild /dev/usb after devices went away (names are handed out again)
void usb_export_all(void);

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
