#include <kernel/arch/x86/bus/pci/pci.h>
#include <kernel/arch/x86/bus/usb/usb.h>
#include <kernel/arch/x86/bus/usb/usb_internal.h>
#include <kernel/arch/x86/virtual/vmm.h>

#include <kernel/malloc.h>
#include <kernel/console/print.h>
#include <kernel/util/string.h>

//
// xHCI (eXtensible Host Controller Interface, USB 3)
//
// Polled: interrupts stay off and the event ring is read after each
// command or transfer. One command ring, one event ring segment, and per
// device a slot with its endpoint 0 transfer ring. Every ring is one page.
// The controller is 64-bit capable but this kernel has no PAE, so every
// address handed to it is below 4 GB and the high halves are zero.
//

// Capability registers
#define XHCI_CAPLENGTH      0x00
#define XHCI_HCSPARAMS1     0x04
#define XHCI_HCSPARAMS2     0x08
#define XHCI_HCCPARAMS1     0x10
#define XHCI_DBOFF          0x14
#define XHCI_RTSOFF         0x18

#define XHCI_HCC_PPC        (1U << 3)   // Port power control
#define XHCI_HCC_CSZ        (1U << 2)   // 64-byte contexts

// Operational registers
#define XHCI_USBCMD         0x00
#define XHCI_USBSTS         0x04
#define XHCI_PAGESIZE       0x08
#define XHCI_DNCTRL         0x14
#define XHCI_CRCR           0x18
#define XHCI_DCBAAP         0x30
#define XHCI_CONFIG         0x38
#define XHCI_PORTSC_BASE    0x400

#define XHCI_CMD_RUN        (1U << 0)
#define XHCI_CMD_HCRST      (1U << 1)
#define XHCI_STS_HCH        (1U << 0)   // Halted
#define XHCI_STS_CNR        (1U << 11)  // Controller not ready

// PORTSC
#define XHCI_PORT_CCS       (1U << 0)
#define XHCI_PORT_PED       (1U << 1)   // Enabled (write 1 disables!)
#define XHCI_PORT_PR        (1U << 4)
#define XHCI_PORT_PP        (1U << 9)
#define XHCI_PORT_SPEED(v)  (((v) >> 10) & 0x0F)
#define XHCI_PORT_CSC       (1U << 17)
#define XHCI_PORT_PEC       (1U << 18)
#define XHCI_PORT_WRC       (1U << 19)
#define XHCI_PORT_OCC       (1U << 20)
#define XHCI_PORT_PRC       (1U << 21)
#define XHCI_PORT_PLC       (1U << 22)
#define XHCI_PORT_CEC       (1U << 23)
#define XHCI_PORT_CHANGES   (XHCI_PORT_CSC | XHCI_PORT_PEC | XHCI_PORT_WRC | XHCI_PORT_OCC | \
                             XHCI_PORT_PRC | XHCI_PORT_PLC | XHCI_PORT_CEC)
// Bits that are written back unchanged: read-only ones and RW (not RW1C/RW1S)
#define XHCI_PORT_RO        ((1U << 0) | (1U << 3) | (0x0FU << 10) | (1U << 30))
#define XHCI_PORT_RWS       ((0x0FU << 5) | (1U << 9) | (0x03U << 14) | (0x07U << 25))

// PSI speed IDs (default mapping, xHCI 7.2.2.1.1)
#define XHCI_SPEED_FULL     1
#define XHCI_SPEED_LOW      2
#define XHCI_SPEED_HIGH     3
#define XHCI_SPEED_SUPER    4

// Runtime registers, interrupter 0
#define XHCI_IR0            0x20
#define XHCI_IMAN           0x00
#define XHCI_IMOD           0x04
#define XHCI_ERSTSZ         0x08
#define XHCI_ERSTBA         0x10
#define XHCI_ERDP           0x18
#define XHCI_ERDP_EHB       (1U << 3)

// Extended capabilities
#define XHCI_EXT_LEGACY     1
#define XHCI_EXT_PROTOCOL   2
#define XHCI_LEGACY_BIOS    (1U << 16)
#define XHCI_LEGACY_OS      (1U << 24)
#define XHCI_LEGACY_KEEP    ((0x7U << 1) | (0xFFU << 5) | (0x7U << 17))  // Everything but the SMI enables
#define XHCI_LEGACY_EVENTS  (0x7U << 29)                                // RW1C SMI status

// TRB types
#define TRB_NORMAL          1
#define TRB_SETUP           2
#define TRB_DATA            3
#define TRB_STATUS          4
#define TRB_LINK            6
#define TRB_ENABLE_SLOT     9
#define TRB_DISABLE_SLOT    10
#define TRB_ADDRESS_DEVICE  11
#define TRB_CONFIGURE_EP    12
#define TRB_EVALUATE_CTX    13
#define TRB_RESET_EP        14
#define TRB_STOP_EP         15
#define TRB_SET_TR_DEQUEUE  16
#define TRB_EV_TRANSFER     32
#define TRB_EV_COMMAND      33
#define TRB_EV_PORT_STATUS  34

#define TRB_TYPE(t)         ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c)     (((c) >> 10) & 0x3F)
#define TRB_CYCLE           (1U << 0)
#define TRB_TOGGLE_CYCLE    (1U << 1)   // Link TRB
#define TRB_ISP             (1U << 2)   // Event on short packet
#define TRB_IOC             (1U << 5)
#define TRB_IDT             (1U << 6)   // Immediate data (setup packet in the TRB)
#define TRB_DIR_IN          (1U << 16)
#define TRB_SLOT(s)         ((uint32_t)(s) << 24)
#define TRB_EP(e)           ((uint32_t)(e) << 16)
#define TRB_GET_SLOT(c)     (((c) >> 24) & 0xFF)
#define TRB_GET_CODE(s)     (((s) >> 24) & 0xFF)

#define TRB_TRT_NONE        (0U << 16)
#define TRB_TRT_OUT         (2U << 16)
#define TRB_TRT_IN          (3U << 16)

// Completion codes
#define XHCI_CC_SUCCESS     1
#define XHCI_CC_STALL       6
#define XHCI_CC_SHORT       13

#define XHCI_RING_TRBS      (PAGE_SIZE / sizeof(struct XhciTrb))    // 256, the last one links back
#define XHCI_MAX_SLOTS      64
#define XHCI_MMIO_MAX       0x100000
#define XHCI_CMD_TIMEOUT    1000
#define XHCI_XFER_TIMEOUT   1000
#define XHCI_EP0            1           // Device context index of endpoint 0

struct __attribute__((packed, aligned(16))) XhciTrb {
    volatile uint32_t param_lo;
    volatile uint32_t param_hi;
    volatile uint32_t status;
    volatile uint32_t control;
};

struct XhciRing {
    struct XhciTrb* trbs;
    uint32_t phys;
    uint32_t enqueue;
    uint32_t cycle;
};

struct __attribute__((packed)) XhciErstEntry {
    uint32_t base_lo;
    uint32_t base_hi;
    uint32_t size;
    uint32_t reserved;
};

struct XhciDevice {
    uint8_t   slot;
    uint8_t   speed_id;         // PSI speed ID written to the slot context
    uint8_t*  input;            // Input context
    uint32_t  input_phys;
    uint8_t*  output;           // Device context the controller owns
    uint32_t  output_phys;
    struct XhciRing ep0;
};

struct XhciState {
    volatile uint8_t* cap;
    volatile uint8_t* op;
    volatile uint8_t* rt;
    volatile uint8_t* db;
    uint32_t mmio_size;

    uint32_t ctx_size;          // 32 or 64 bytes
    uint8_t  max_slots;
    uint8_t  max_ports;

    uint64_t* dcbaa;
    uint32_t  dcbaa_phys;

    struct XhciRing cmd;

    struct XhciTrb* events;
    uint32_t events_phys;
    uint32_t event_dequeue;
    uint32_t event_cycle;

    uint8_t* data;              // Data stage bounce buffer (USB_CONTROL_MAX)
    uint32_t data_phys;

    uint8_t  port_major[256];   // USB major revision per port (2, 3), 0 = unknown
    uint8_t  port_speed[256];   // PSI speed ID seen at the last port reset
};

//
// Register access
//

static inline uint32_t xhci_read(volatile uint8_t* base, uint32_t offset) {
    return *(volatile uint32_t*)(base + offset);
}

static inline void xhci_write(volatile uint8_t* base, uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)(base + offset) = value;
}

// 64-bit registers take the low half first
static inline void xhci_write64(volatile uint8_t* base, uint32_t offset, uint32_t value) {
    xhci_write(base, offset, value);
    xhci_write(base, offset + 4, 0);
}

static inline volatile uint8_t* xhci_portsc(struct XhciState* st, uint8_t port) {
    return st->op + XHCI_PORTSC_BASE + 0x10U * (port - 1);
}

static bool xhci_wait(volatile uint8_t* base, uint32_t offset, uint32_t mask, uint32_t want, uint32_t timeout_ms) {
    uint64_t start = usb_time_ms();
    while ((xhci_read(base, offset) & mask) != want) {
        if (usb_time_ms() - start > timeout_ms) return false;
    }
    return true;
}

//
// Contexts
//

static inline uint32_t* xhci_input_control(struct XhciState* st, struct XhciDevice* xd) {
    (void)st;
    return (uint32_t*)xd->input;
}

static inline uint32_t* xhci_input_slot(struct XhciState* st, struct XhciDevice* xd) {
    return (uint32_t*)(xd->input + st->ctx_size);
}

static inline uint32_t* xhci_input_ep(struct XhciState* st, struct XhciDevice* xd, uint32_t dci) {
    return (uint32_t*)(xd->input + st->ctx_size * (dci + 1));
}

static inline uint32_t* xhci_output_slot(struct XhciState* st, struct XhciDevice* xd) {
    (void)st;
    return (uint32_t*)xd->output;
}

//
// Rings
//

static bool xhci_ring_init(struct XhciRing* ring) {
    ring->trbs = (struct XhciTrb*)usb_dma_page(&ring->phys);
    ring->enqueue = 0;
    ring->cycle = 1;
    return ring->trbs != NULL;
}

// Queue one TRB; returns its physical address. The cycle bit is written
// last, which is what hands the TRB to the controller.
static uint32_t xhci_ring_push(struct XhciRing* ring, uint32_t param_lo, uint32_t param_hi,
                               uint32_t status, uint32_t control) {
    struct XhciTrb* trb = &ring->trbs[ring->enqueue];
    uint32_t phys = ring->phys + ring->enqueue * (uint32_t)sizeof(struct XhciTrb);

    trb->param_lo = param_lo;
    trb->param_hi = param_hi;
    trb->status   = status;
    usb_mmio_barrier();
    trb->control  = (control & ~TRB_CYCLE) | ring->cycle;

    if (++ring->enqueue == XHCI_RING_TRBS - 1) {
        struct XhciTrb* link = &ring->trbs[XHCI_RING_TRBS - 1];
        link->param_lo = ring->phys;
        link->param_hi = 0;
        link->status   = 0;
        usb_mmio_barrier();
        link->control  = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE | ring->cycle;

        ring->enqueue = 0;
        ring->cycle ^= 1;
    }
    return phys;
}

static inline uint32_t xhci_ring_dequeue_ptr(const struct XhciRing* ring) {
    return (ring->phys + ring->enqueue * (uint32_t)sizeof(struct XhciTrb)) | ring->cycle;
}

static inline void xhci_doorbell(struct XhciState* st, uint8_t slot, uint32_t target) {
    usb_mmio_barrier();
    xhci_write(st->db, 4U * slot, target);
}

//
// Events
//

static bool xhci_next_event(struct XhciState* st, struct XhciTrb* out) {
    struct XhciTrb* trb = &st->events[st->event_dequeue];
    if ((trb->control & TRB_CYCLE) != st->event_cycle)
        return false;

    out->param_lo = trb->param_lo;
    out->param_hi = trb->param_hi;
    out->status   = trb->status;
    out->control  = trb->control;

    if (++st->event_dequeue == XHCI_RING_TRBS) {
        st->event_dequeue = 0;
        st->event_cycle ^= 1;
    }
    xhci_write64(st->rt, XHCI_IR0 + XHCI_ERDP,
                 (st->events_phys + st->event_dequeue * (uint32_t)sizeof(struct XhciTrb)) | XHCI_ERDP_EHB);
    return true;
}

// Run one command and wait for its completion event.
// Returns the completion code (0 on timeout); *slot_out gets the slot ID.
static uint32_t xhci_command(struct XhciState* st, uint32_t param_lo, uint32_t control, uint8_t* slot_out) {
    uint32_t trb_phys = xhci_ring_push(&st->cmd, param_lo, 0, 0, control);
    xhci_doorbell(st, 0, 0);

    uint64_t start = usb_time_ms();
    while (usb_time_ms() - start < XHCI_CMD_TIMEOUT) {
        struct XhciTrb ev;
        if (!xhci_next_event(st, &ev))
            continue;
        if (TRB_GET_TYPE(ev.control) == TRB_EV_COMMAND && ev.param_lo == trb_phys) {
            if (slot_out != NULL)
                *slot_out = (uint8_t)TRB_GET_SLOT(ev.control);
            return TRB_GET_CODE(ev.status);
        }
        // Port status changes and stray transfer events: nothing to do
    }
    print("xHCI: command timed out\n");
    return 0;
}

//
// Controller start
//

// Intel 7/8/9-series chipsets route their USB 3 capable ports to EHCI
// until told otherwise
static void xhci_intel_port_switch(const struct PCIDeviceInfo* pci) {
    if (pci->vendor_id != 0x8086)
        return;
    switch (pci->device_id) {
        case 0x1E31: case 0x8C31: case 0x9C31: case 0x9CB1: case 0x8CB1:
            break;
        default:
            return;
    }
    // USB3_PSSEN <- USB3PRM, XUSB2PR <- XUSB2PRM
    pci_config_write(pci->bus, pci->dev, pci->func, 0xD8, pci_config_read(pci->bus, pci->dev, pci->func, 0xDC));
    pci_config_write(pci->bus, pci->dev, pci->func, 0xD0, pci_config_read(pci->bus, pci->dev, pci->func, 0xD4));
}

// Walk the extended capabilities: take the controller from the BIOS and
// note which ports are USB 2 and which USB 3
static void xhci_scan_extended_caps(struct XhciState* st) {
    uint32_t offset = (xhci_read(st->cap, XHCI_HCCPARAMS1) >> 16) << 2;

    for (uint32_t guard = 0; offset != 0 && offset + 12 <= st->mmio_size && guard < 64; guard++) {
        uint32_t header = xhci_read(st->cap, offset);
        uint8_t  id = (uint8_t)(header & 0xFF);

        if (id == XHCI_EXT_LEGACY) {
            if (header & XHCI_LEGACY_BIOS) {
                st->cap[offset + 3] = 1;    // OS owned semaphore
                uint64_t start = usb_time_ms();
                while (xhci_read(st->cap, offset) & XHCI_LEGACY_BIOS) {
                    if (usb_time_ms() - start > 1000) {
                        print("xHCI: BIOS did not release the controller\n");
                        break;
                    }
                }
            }
            uint32_t ctl = xhci_read(st->cap, offset + 4);
            xhci_write(st->cap, offset + 4, (ctl & XHCI_LEGACY_KEEP) | XHCI_LEGACY_EVENTS);
        } else if (id == XHCI_EXT_PROTOCOL) {
            uint8_t  major = (uint8_t)(header >> 24);
            uint32_t ports = xhci_read(st->cap, offset + 8);
            uint32_t first = ports & 0xFF;
            uint32_t count = (ports >> 8) & 0xFF;
            for (uint32_t p = first; p < first + count && p < 256; p++)
                st->port_major[p] = major;
        }

        uint32_t next = (header >> 8) & 0xFF;
        offset = next ? offset + (next << 2) : 0;
    }
}

static bool xhci_start(struct UsbController* hc) {
    const struct PCIDeviceInfo* pci = &hc->pci;
    pci_enable_command_bits(pci->bus, pci->dev, pci->func, PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER);

    struct PCIBar bar;
    if (pci_read_bar(pci->bus, pci->dev, pci->func, 0, &bar) == 0 || bar.size == 0)
        return false;

    uint32_t mmio_size = bar.size > XHCI_MMIO_MAX ? XHCI_MMIO_MAX : (uint32_t)bar.size;
    volatile uint8_t* cap = (volatile uint8_t*)pci_map_bar(pci->bus, pci->dev, pci->func, 0, mmio_size,
                                                           VM_PRESENT | VM_READWRITE | VM_PCD);
    if (cap == NULL) {
        print("xHCI: registers not mappable (above 4 GB?)\n");
        return false;
    }

    struct XhciState* st = (struct XhciState*)malloc(sizeof(struct XhciState));
    if (st == NULL) return false;
    memset(st, 0, sizeof(*st));

    st->cap = cap;
    st->mmio_size = mmio_size;
    st->op = cap + cap[XHCI_CAPLENGTH];
    st->rt = cap + (xhci_read(cap, XHCI_RTSOFF) & ~0x1FU);
    st->db = cap + (xhci_read(cap, XHCI_DBOFF) & ~0x3U);

    uint32_t hcs1 = xhci_read(cap, XHCI_HCSPARAMS1);
    uint32_t hcs2 = xhci_read(cap, XHCI_HCSPARAMS2);
    uint32_t hcc1 = xhci_read(cap, XHCI_HCCPARAMS1);
    st->max_slots = (uint8_t)(hcs1 & 0xFF);
    st->max_ports = (uint8_t)(hcs1 >> 24);
    st->ctx_size  = (hcc1 & XHCI_HCC_CSZ) ? 64 : 32;
    if (st->max_slots > XHCI_MAX_SLOTS)
        st->max_slots = XHCI_MAX_SLOTS;

    xhci_intel_port_switch(pci);
    xhci_scan_extended_caps(st);

    // Stop, then reset
    if (!xhci_wait(st->op, XHCI_USBSTS, XHCI_STS_CNR, 0, 1000))
        goto fail;
    xhci_write(st->op, XHCI_USBCMD, xhci_read(st->op, XHCI_USBCMD) & ~XHCI_CMD_RUN);
    if (!xhci_wait(st->op, XHCI_USBSTS, XHCI_STS_HCH, XHCI_STS_HCH, 100))
        goto fail;
    xhci_write(st->op, XHCI_USBCMD, XHCI_CMD_HCRST);
    if (!xhci_wait(st->op, XHCI_USBCMD, XHCI_CMD_HCRST, 0, 1000) ||
        !xhci_wait(st->op, XHCI_USBSTS, XHCI_STS_CNR, 0, 1000)) {
        print("xHCI: controller reset timed out\n");
        goto fail;
    }
    usb_delay_ms(10);

    if (!(xhci_read(st->op, XHCI_PAGESIZE) & 0x1)) {
        print("xHCI: controller does not support 4 KB pages\n");
        goto fail;
    }

    // Device context base array, with the scratchpad buffers the
    // controller asks for in entry 0
    st->dcbaa = (uint64_t*)usb_dma_page(&st->dcbaa_phys);
    if (st->dcbaa == NULL) goto fail;

    uint32_t scratchpads = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);
    if (scratchpads > 0) {
        uint32_t array_phys = 0;
        uint64_t* array = (uint64_t*)usb_dma_page(&array_phys);
        if (array == NULL || scratchpads > PAGE_SIZE / sizeof(uint64_t)) goto fail;
        for (uint32_t i = 0; i < scratchpads; i++) {
            uint32_t buf_phys = 0;
            if (usb_dma_page(&buf_phys) == NULL) goto fail;
            array[i] = buf_phys;
        }
        st->dcbaa[0] = array_phys;
    }

    // Command ring
    if (!xhci_ring_init(&st->cmd)) goto fail;
    xhci_write64(st->op, XHCI_CRCR, st->cmd.phys | st->cmd.cycle);

    // Event ring: one segment
    uint32_t erst_phys = 0;
    struct XhciErstEntry* erst = (struct XhciErstEntry*)usb_dma_page(&erst_phys);
    st->events = (struct XhciTrb*)usb_dma_page(&st->events_phys);
    st->data   = (uint8_t*)usb_dma_page(&st->data_phys);
    if (erst == NULL || st->events == NULL || st->data == NULL) goto fail;

    erst[0].base_lo = st->events_phys;
    erst[0].base_hi = 0;
    erst[0].size    = XHCI_RING_TRBS;
    st->event_dequeue = 0;
    st->event_cycle   = 1;

    volatile uint8_t* ir = st->rt + XHCI_IR0;
    xhci_write(ir, XHCI_IMAN, 0);
    xhci_write(ir, XHCI_ERSTSZ, 1);
    xhci_write64(ir, XHCI_ERDP, st->events_phys);
    xhci_write64(ir, XHCI_ERSTBA, erst_phys);

    xhci_write64(st->op, XHCI_DCBAAP, st->dcbaa_phys);
    xhci_write(st->op, XHCI_CONFIG, st->max_slots);
    xhci_write(st->op, XHCI_DNCTRL, 0);

    xhci_write(st->op, XHCI_USBCMD, XHCI_CMD_RUN);
    if (!xhci_wait(st->op, XHCI_USBSTS, XHCI_STS_HCH, 0, 100)) {
        print("xHCI: controller did not start\n");
        goto fail;
    }

    // Power every port, then give connections time to debounce and USB 3
    // links time to train
    if (hcc1 & XHCI_HCC_PPC) {
        for (uint8_t port = 1; port <= st->max_ports; port++) {
            volatile uint8_t* portsc = xhci_portsc(st, port);
            uint32_t v = xhci_read(portsc, 0);
            if (!(v & XHCI_PORT_PP))
                xhci_write(portsc, 0, (v & (XHCI_PORT_RO | XHCI_PORT_RWS)) | XHCI_PORT_PP);
        }
        usb_delay_ms(20);
    }
    usb_delay_ms(100);

    hc->port_count = st->max_ports;
    hc->priv = st;
    return true;

fail:
    // Pages handed to the controller are not reclaimed: it may still own them
    print("xHCI: controller setup failed\n");
    free(st);
    return false;
}

//
// Ports
//

static uint8_t xhci_speed_from_id(uint8_t id) {
    switch (id) {
        case XHCI_SPEED_LOW:  return USB_SPEED_LOW;
        case XHCI_SPEED_FULL: return USB_SPEED_FULL;
        case XHCI_SPEED_HIGH: return USB_SPEED_HIGH;
        default:              return id >= XHCI_SPEED_SUPER ? USB_SPEED_SUPER : 0;
    }
}

static uint8_t xhci_id_from_speed(uint8_t speed) {
    switch (speed) {
        case USB_SPEED_LOW:   return XHCI_SPEED_LOW;
        case USB_SPEED_FULL:  return XHCI_SPEED_FULL;
        case USB_SPEED_HIGH:  return XHCI_SPEED_HIGH;
        default:              return XHCI_SPEED_SUPER;
    }
}

static inline uint32_t xhci_port_neutral(uint32_t v) {
    return v & (XHCI_PORT_RO | XHCI_PORT_RWS);
}

static uint8_t xhci_port_reset(struct UsbController* hc, uint8_t port) {
    struct XhciState* st = (struct XhciState*)hc->priv;
    volatile uint8_t* portsc = xhci_portsc(st, port);

    uint32_t v = xhci_read(portsc, 0);
    if (!(v & XHCI_PORT_CCS))
        return 0;

    // USB 3 ports enable themselves once the link trains. USB 2 ports
    // (and USB 3 ones that did not come up) need a reset.
    if (st->port_major[port] == 3 && !(v & XHCI_PORT_PED)) {
        uint64_t start = usb_time_ms();
        while (!(xhci_read(portsc, 0) & XHCI_PORT_PED) && usb_time_ms() - start < 300) { }
        v = xhci_read(portsc, 0);
    }

    if (!(v & XHCI_PORT_PED) || st->port_major[port] != 3) {
        xhci_write(portsc, 0, xhci_port_neutral(v) | XHCI_PORT_PR);

        uint64_t start = usb_time_ms();
        while (!(xhci_read(portsc, 0) & XHCI_PORT_PRC)) {
            if (usb_time_ms() - start > 500) {
                print("xHCI: port reset timed out\n");
                return 0;
            }
        }
        usb_delay_ms(10);   // Reset recovery
    }

    // Acknowledge every change bit so the port stays quiet
    v = xhci_read(portsc, 0);
    xhci_write(portsc, 0, xhci_port_neutral(v) | (v & XHCI_PORT_CHANGES));

    if (!(v & XHCI_PORT_CCS) || !(v & XHCI_PORT_PED))
        return 0;

    st->port_speed[port] = (uint8_t)XHCI_PORT_SPEED(v);
    return xhci_speed_from_id(st->port_speed[port]);
}

//
// Devices
//

static void xhci_free_device(struct XhciState* st, struct XhciDevice* xd) {
    if (xd == NULL) return;
    if (xd->slot != 0) {
        xhci_command(st, 0, TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(xd->slot), NULL);
        st->dcbaa[xd->slot] = 0;
    }
    usb_dma_page_free(xd->input);
    usb_dma_page_free(xd->output);
    usb_dma_page_free(xd->ep0.trbs);
    free(xd);
}

static void xhci_release_device(struct UsbController* hc, struct UsbDevice* dev) {
    xhci_free_device((struct XhciState*)hc->priv, (struct XhciDevice*)dev->hcd_data);
    dev->hcd_data = NULL;
}

// The nearest high-speed hub above a low/full speed device does its
// transaction translation; its slot and our port on it go in the slot context
static void xhci_find_tt(const struct UsbDevice* dev, uint8_t* tt_slot, uint8_t* tt_port, bool* mtt) {
    *tt_slot = 0;
    *tt_port = 0;
    *mtt = false;
    if (dev->speed != USB_SPEED_LOW && dev->speed != USB_SPEED_FULL)
        return;

    for (const struct UsbDevice* d = dev; d->parent != NULL; d = d->parent) {
        if (d->parent->speed == USB_SPEED_HIGH) {
            const struct XhciDevice* hub = (const struct XhciDevice*)d->parent->hcd_data;
            *tt_slot = hub != NULL ? hub->slot : 0;
            *tt_port = d->port;
            *mtt = d->parent->hub_multi_tt;
            return;
        }
    }
}

static void xhci_fill_ep0(struct XhciState* st, struct XhciDevice* xd, uint16_t max_packet) {
    uint32_t* ep0 = xhci_input_ep(st, xd, XHCI_EP0);
    ep0[0] = 0;
    ep0[1] = (3U << 1) | (4U << 3) | ((uint32_t)max_packet << 16);   // CErr 3, control endpoint
    ep0[2] = xhci_ring_dequeue_ptr(&xd->ep0);
    ep0[3] = 0;
    ep0[4] = 8;                                                       // Average TRB length
}

static bool xhci_address_device(struct UsbController* hc, struct UsbDevice* dev) {
    struct XhciState* st = (struct XhciState*)hc->priv;

    struct XhciDevice* xd = (struct XhciDevice*)malloc(sizeof(struct XhciDevice));
    if (xd == NULL) return false;
    memset(xd, 0, sizeof(*xd));
    dev->hcd_data = xd;

    xd->input  = (uint8_t*)usb_dma_page(&xd->input_phys);
    xd->output = (uint8_t*)usb_dma_page(&xd->output_phys);
    if (xd->input == NULL || xd->output == NULL || !xhci_ring_init(&xd->ep0))
        goto fail;

    uint8_t slot = 0;
    uint32_t code = xhci_command(st, 0, TRB_TYPE(TRB_ENABLE_SLOT), &slot);
    if (code != XHCI_CC_SUCCESS || slot == 0 || slot > st->max_slots) {
        print("xHCI: no free device slot\n");
        goto fail;
    }
    xd->slot = slot;
    st->dcbaa[slot] = xd->output_phys;

    xd->speed_id = dev->parent == NULL ? st->port_speed[dev->root_port] : xhci_id_from_speed(dev->speed);
    if (xd->speed_id == 0)
        xd->speed_id = xhci_id_from_speed(dev->speed);

    uint8_t tt_slot, tt_port;
    bool mtt;
    xhci_find_tt(dev, &tt_slot, &tt_port, &mtt);

    // Add the slot context and endpoint 0
    uint32_t* icc = xhci_input_control(st, xd);
    icc[0] = 0;
    icc[1] = (1U << 0) | (1U << 1);

    uint32_t* slot_ctx = xhci_input_slot(st, xd);
    slot_ctx[0] = (dev->route & 0xFFFFF) | ((uint32_t)xd->speed_id << 20) | (mtt ? (1U << 25) : 0) | (1U << 27);
    slot_ctx[1] = (uint32_t)dev->root_port << 16;
    slot_ctx[2] = tt_slot | ((uint32_t)tt_port << 8);
    slot_ctx[3] = 0;

    xhci_fill_ep0(st, xd, dev->max_packet0);

    code = xhci_command(st, xd->input_phys, TRB_TYPE(TRB_ADDRESS_DEVICE) | TRB_SLOT(slot), NULL);
    if (code != XHCI_CC_SUCCESS) {
        print("xHCI: address device failed, code ");
        print_int((int32_t)code);
        print("\n");
        goto fail;
    }

    dev->address = (uint8_t)(xhci_output_slot(st, xd)[3] & 0xFF);
    return true;

fail:
    xhci_free_device(st, xd);
    dev->hcd_data = NULL;
    return false;
}

static bool xhci_update_max_packet0(struct UsbController* hc, struct UsbDevice* dev) {
    struct XhciState* st = (struct XhciState*)hc->priv;
    struct XhciDevice* xd = (struct XhciDevice*)dev->hcd_data;
    if (xd == NULL) return false;

    uint32_t* icc = xhci_input_control(st, xd);
    icc[0] = 0;
    icc[1] = 1U << 1;   // Endpoint 0 only

    uint32_t* ep0 = xhci_input_ep(st, xd, XHCI_EP0);
    ep0[1] = (ep0[1] & 0x0000FFFFU) | ((uint32_t)dev->max_packet0 << 16);

    return xhci_command(st, xd->input_phys, TRB_TYPE(TRB_EVALUATE_CTX) | TRB_SLOT(xd->slot), NULL) == XHCI_CC_SUCCESS;
}

// Tell the controller this slot is a hub, so devices below it (and their
// transaction translators) can be addressed
static void xhci_hub_configured(struct UsbController* hc, struct UsbDevice* dev) {
    struct XhciState* st = (struct XhciState*)hc->priv;
    struct XhciDevice* xd = (struct XhciDevice*)dev->hcd_data;
    if (xd == NULL) return;

    uint32_t* icc = xhci_input_control(st, xd);
    icc[0] = 0;
    icc[1] = 1U << 0;   // Slot context only

    uint32_t* slot_ctx = xhci_input_slot(st, xd);
    memcpy(slot_ctx, xhci_output_slot(st, xd), st->ctx_size);
    slot_ctx[0] |= 1U << 26;                                    // Hub
    if (dev->hub_multi_tt)
        slot_ctx[0] |= 1U << 25;                                // Multi-TT
    slot_ctx[1] = (slot_ctx[1] & 0x00FFFFFFU) | ((uint32_t)dev->hub_ports << 24);
    if (dev->speed == USB_SPEED_HIGH)
        slot_ctx[2] = (slot_ctx[2] & ~(3U << 16)) | ((uint32_t)dev->hub_think_time << 16);
    slot_ctx[3] = 0;

    if (xhci_command(st, xd->input_phys, TRB_TYPE(TRB_CONFIGURE_EP) | TRB_SLOT(xd->slot), NULL) != XHCI_CC_SUCCESS)
        print("xHCI: could not mark a hub, devices behind it may not work\n");
}

// After a stall or a timeout: halt endpoint 0 if needed and move its
// dequeue pointer past whatever is left of the failed transfer
static void xhci_recover_ep0(struct XhciState* st, struct XhciDevice* xd, bool stalled) {
    if (stalled)
        xhci_command(st, 0, TRB_TYPE(TRB_RESET_EP) | TRB_SLOT(xd->slot) | TRB_EP(XHCI_EP0), NULL);
    else
        xhci_command(st, 0, TRB_TYPE(TRB_STOP_EP) | TRB_SLOT(xd->slot) | TRB_EP(XHCI_EP0), NULL);

    xhci_command(st, xhci_ring_dequeue_ptr(&xd->ep0),
                 TRB_TYPE(TRB_SET_TR_DEQUEUE) | TRB_SLOT(xd->slot) | TRB_EP(XHCI_EP0), NULL);
}

static int xhci_control(struct UsbController* hc, struct UsbDevice* dev,
                        const struct UsbSetupPacket* setup, void* data) {
    struct XhciState* st = (struct XhciState*)hc->priv;
    struct XhciDevice* xd = (struct XhciDevice*)dev->hcd_data;
    if (xd == NULL)
        return USB_ERR_PARAM;

    uint32_t length = setup->length;
    bool in = (setup->request_type & USB_DIR_IN) != 0;
    if (length > USB_CONTROL_MAX)
        return USB_ERR_PARAM;
    if (!in && length > 0)
        memcpy(st->data, data, length);

    const uint8_t* raw = (const uint8_t*)setup;
    uint32_t setup_lo = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) | ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24);
    uint32_t setup_hi = (uint32_t)raw[4] | ((uint32_t)raw[5] << 8) | ((uint32_t)raw[6] << 16) | ((uint32_t)raw[7] << 24);
    uint32_t trt = length == 0 ? TRB_TRT_NONE : (in ? TRB_TRT_IN : TRB_TRT_OUT);

    xhci_ring_push(&xd->ep0, setup_lo, setup_hi, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | trt);

    uint32_t data_trb = 0;
    if (length > 0)
        data_trb = xhci_ring_push(&xd->ep0, st->data_phys, 0, length,
                                  TRB_TYPE(TRB_DATA) | TRB_ISP | (in ? TRB_DIR_IN : 0));

    // Status stage runs opposite to the data stage (IN when there is none)
    uint32_t status_trb = xhci_ring_push(&xd->ep0, 0, 0, 0,
                                         TRB_TYPE(TRB_STATUS) | TRB_IOC | ((length == 0 || !in) ? TRB_DIR_IN : 0));

    xhci_doorbell(st, xd->slot, XHCI_EP0);

    uint32_t residual = 0;
    int result = USB_ERR_TIMEOUT;
    uint64_t start = usb_time_ms();
    while (usb_time_ms() - start < XHCI_XFER_TIMEOUT) {
        struct XhciTrb ev;
        if (!xhci_next_event(st, &ev))
            continue;
        if (TRB_GET_TYPE(ev.control) != TRB_EV_TRANSFER || TRB_GET_SLOT(ev.control) != xd->slot)
            continue;

        uint32_t code = TRB_GET_CODE(ev.status);
        if (code == XHCI_CC_SHORT && ev.param_lo == data_trb) {
            residual = ev.status & 0x00FFFFFF;
            continue;   // The status stage still follows
        }
        if (code == XHCI_CC_SUCCESS && ev.param_lo == status_trb) {
            result = 0;
            break;
        }
        if (code == XHCI_CC_SUCCESS)
            continue;

        result = (code == XHCI_CC_STALL) ? USB_ERR_STALL : USB_ERR_IO;
        break;
    }

    if (result < 0) {
        xhci_recover_ep0(st, xd, result == USB_ERR_STALL);
        return result;
    }

    uint32_t moved = residual > length ? 0 : length - residual;
    if (in && moved > 0)
        memcpy(data, st->data, moved);
    return (int)moved;
}

const struct UsbHcdOps xhci_ops = {
    .name               = "xhci",
    .start              = xhci_start,
    .port_reset         = xhci_port_reset,
    .address_device     = xhci_address_device,
    .update_max_packet0 = xhci_update_max_packet0,
    .control            = xhci_control,
    .hub_configured     = xhci_hub_configured,
    .release_device     = xhci_release_device,
};
