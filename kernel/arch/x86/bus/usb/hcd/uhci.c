#include <kernel/arch/x86/io.h>
#include <kernel/arch/x86/virtual/pmm.h>
#include <kernel/arch/x86/bus/pci/pci.h>
#include <kernel/arch/x86/bus/usb/usb.h>
#include <kernel/arch/x86/bus/usb/usb_internal.h>

#include <kernel/malloc.h>
#include <kernel/console/print.h>
#include <kernel/util/string.h>

//
// UHCI (Universal Host Controller Interface, USB 1.1)
//
// One queue head sits in every slot of the frame list. A control transfer
// is a chain of transfer descriptors hung from that queue head, polled until
// the last one retires. Low and full speed only.
//

// I/O registers (BAR4)
#define UHCI_USBCMD         0x00
#define UHCI_USBSTS         0x02
#define UHCI_USBINTR        0x04
#define UHCI_FRNUM          0x06
#define UHCI_FRBASEADD      0x08
#define UHCI_SOFMOD         0x0C
#define UHCI_PORTSC1        0x10

#define UHCI_CMD_RS         0x0001  // Run
#define UHCI_CMD_HCRESET    0x0002
#define UHCI_CMD_GRESET     0x0004
#define UHCI_CMD_CF         0x0040  // Configured (software flag)
#define UHCI_CMD_MAXP       0x0080  // 64-byte reclamation packets

#define UHCI_STS_HCHALTED   0x0020

#define UHCI_PORT_CCS       0x0001  // Device connected
#define UHCI_PORT_CSC       0x0002  // Connect change (RWC)
#define UHCI_PORT_PE        0x0004  // Enabled
#define UHCI_PORT_PEC       0x0008  // Enable change (RWC)
#define UHCI_PORT_VALID     0x0080  // Reserved, reads as 1 on a real port
#define UHCI_PORT_LSDA      0x0100  // Low speed device attached
#define UHCI_PORT_PR        0x0200  // Reset

// PCI config: legacy support (keyboard emulation via SMI)
#define UHCI_PCI_LEGSUP     0xC0
#define UHCI_LEGSUP_RWC     0x8F00  // Clears the SMI status bits, SMIs off

// Link pointers
#define UHCI_LINK_TERMINATE 0x00000001U
#define UHCI_LINK_QH        0x00000002U
#define UHCI_LINK_DEPTH     0x00000004U     // Depth first: run the next TD now

// TD control/status
#define UHCI_TD_ACTLEN_MASK 0x000007FFU
#define UHCI_TD_BITSTUFF    (1U << 17)
#define UHCI_TD_CRC_TIMEOUT (1U << 18)
#define UHCI_TD_NAK         (1U << 19)
#define UHCI_TD_BABBLE      (1U << 20)
#define UHCI_TD_BUFFER_ERR  (1U << 21)
#define UHCI_TD_STALLED     (1U << 22)
#define UHCI_TD_ACTIVE      (1U << 23)
#define UHCI_TD_LS          (1U << 26)
#define UHCI_TD_CERR_3      (3U << 27)
#define UHCI_TD_SPD         (1U << 29)
#define UHCI_TD_ERRORS      (UHCI_TD_BITSTUFF | UHCI_TD_CRC_TIMEOUT | UHCI_TD_BABBLE | \
                             UHCI_TD_BUFFER_ERR | UHCI_TD_STALLED)

// TD token
#define UHCI_PID_SETUP      0x2D
#define UHCI_PID_IN         0x69
#define UHCI_PID_OUT        0xE1
#define UHCI_TOKEN_TOGGLE   (1U << 19)

#define UHCI_MAX_PORTS      7
#define UHCI_TIMEOUT_MS     1000

// Work page layout: queue head, setup packet, then the TD chain
#define UHCI_QH_OFFSET      0
#define UHCI_SETUP_OFFSET   32
#define UHCI_TD_OFFSET      64
#define UHCI_MAX_TDS        ((PAGE_SIZE - UHCI_TD_OFFSET) / sizeof(struct UhciTd))

struct __attribute__((packed, aligned(16))) UhciTd {
    volatile uint32_t link;
    volatile uint32_t status;
    volatile uint32_t token;
    volatile uint32_t buffer;
};

struct __attribute__((packed, aligned(16))) UhciQh {
    volatile uint32_t head;
    volatile uint32_t element;
};

struct UhciState {
    uint16_t  io;
    uint32_t* frame_list;
    uint8_t*  work;             // Queue head, setup packet, TDs
    uint32_t  work_phys;
    uint8_t*  data;             // Data stage bounce buffer (USB_CONTROL_MAX)
    uint32_t  data_phys;
};

static inline struct UhciQh* uhci_qh(struct UhciState* st) {
    return (struct UhciQh*)(st->work + UHCI_QH_OFFSET);
}

static inline struct UhciTd* uhci_td(struct UhciState* st, uint32_t i) {
    return (struct UhciTd*)(st->work + UHCI_TD_OFFSET) + i;
}

static inline uint32_t uhci_td_phys(struct UhciState* st, uint32_t i) {
    return st->work_phys + UHCI_TD_OFFSET + i * (uint32_t)sizeof(struct UhciTd);
}

static bool uhci_wait_reg_clear(uint16_t port, uint16_t bits, uint32_t timeout_ms) {
    uint64_t start = usb_time_ms();
    while (inw(port) & bits) {
        if (usb_time_ms() - start > timeout_ms) return false;
    }
    return true;
}

static bool uhci_start(struct UsbController* hc) {
    const struct PCIDeviceInfo* pci = &hc->pci;

    uint16_t io = pci_get_io_bar(pci->bus, pci->dev, pci->func, 4);
    if (io == 0) {
        print("UHCI: no I/O BAR\n");
        return false;
    }
    pci_enable_command_bits(pci->bus, pci->dev, pci->func, PCI_COMMAND_IO | PCI_COMMAND_BUS_MASTER);

    // Take it from the BIOS: no more keyboard emulation SMIs
    pci_config_write16(pci->bus, pci->dev, pci->func, UHCI_PCI_LEGSUP, UHCI_LEGSUP_RWC);

    outw(io + UHCI_USBINTR, 0);
    outw(io + UHCI_USBCMD, 0);
    usb_delay_ms(2);

    // Global reset resets the ports too, then the controller itself
    outw(io + UHCI_USBCMD, UHCI_CMD_GRESET);
    usb_delay_ms(50);
    outw(io + UHCI_USBCMD, 0);
    usb_delay_ms(10);

    outw(io + UHCI_USBCMD, UHCI_CMD_HCRESET);
    if (!uhci_wait_reg_clear(io + UHCI_USBCMD, UHCI_CMD_HCRESET, 50)) {
        print("UHCI: controller reset timed out\n");
        return false;
    }

    struct UhciState* st = (struct UhciState*)malloc(sizeof(struct UhciState));
    if (st == NULL) return false;
    memset(st, 0, sizeof(*st));
    st->io = io;

    uint32_t frame_list_phys = 0;
    st->frame_list = (uint32_t*)usb_dma_page(&frame_list_phys);
    st->work       = (uint8_t*)usb_dma_page(&st->work_phys);
    st->data       = (uint8_t*)usb_dma_page(&st->data_phys);
    if (st->frame_list == NULL || st->work == NULL || st->data == NULL) {
        usb_dma_page_free(st->frame_list);
        usb_dma_page_free(st->work);
        usb_dma_page_free(st->data);
        free(st);
        print("UHCI: out of memory\n");
        return false;
    }

    // Every frame visits the one control queue head
    struct UhciQh* qh = uhci_qh(st);
    qh->head    = UHCI_LINK_TERMINATE;
    qh->element = UHCI_LINK_TERMINATE;
    uint32_t qh_link = (st->work_phys + UHCI_QH_OFFSET) | UHCI_LINK_QH;
    for (uint32_t i = 0; i < 1024; i++)
        st->frame_list[i] = qh_link;

    outw(io + UHCI_USBINTR, 0);
    outw(io + UHCI_FRNUM, 0);
    outl(io + UHCI_FRBASEADD, frame_list_phys);
    outb(io + UHCI_SOFMOD, 0x40);   // 1 ms frames
    outw(io + UHCI_USBSTS, 0xFFFF); // Clear every status bit
    outw(io + UHCI_USBCMD, UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP);
    usb_delay_ms(2);

    if (inw(io + UHCI_USBSTS) & UHCI_STS_HCHALTED) {
        print("UHCI: controller did not start\n");
        return false;
    }

    // Ports answer with bit 7 set; past the last one, reads float or are 0
    uint8_t ports = 0;
    while (ports < UHCI_MAX_PORTS) {
        uint16_t v = inw((uint16_t)(io + UHCI_PORTSC1 + ports * 2));
        if (!(v & UHCI_PORT_VALID) || v == 0xFFFF) break;
        ports++;
    }
    if (ports < 2) ports = 2;   // Every UHCI has at least two

    hc->port_count = ports;
    hc->priv = st;
    return true;
}

static uint8_t uhci_port_reset(struct UsbController* hc, uint8_t port) {
    struct UhciState* st = (struct UhciState*)hc->priv;
    uint16_t reg = (uint16_t)(st->io + UHCI_PORTSC1 + (port - 1) * 2);

    if (!(inw(reg) & UHCI_PORT_CCS))
        return 0;

    // Reset for 50 ms (USB 2.0 7.1.7.5), then enable. The enable can need
    // several tries while the connect/enable change bits are still set.
    outw(reg, UHCI_PORT_PR);
    usb_delay_ms(50);
    outw(reg, 0);
    usb_delay_ms(1);

    for (int attempt = 0; attempt < 10; attempt++) {
        uint16_t v = inw(reg);
        if (!(v & UHCI_PORT_CCS))
            return 0;

        if (v & (UHCI_PORT_CSC | UHCI_PORT_PEC)) {
            outw(reg, (uint16_t)((v & UHCI_PORT_PE) | UHCI_PORT_CSC | UHCI_PORT_PEC));
            continue;
        }
        if (v & UHCI_PORT_PE) {
            usb_delay_ms(10);   // Reset recovery
            return (v & UHCI_PORT_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
        }

        outw(reg, UHCI_PORT_PE);
        usb_delay_ms(10);
    }
    return 0;
}

static inline uint32_t uhci_actual_length(uint32_t status) {
    return ((status & UHCI_TD_ACTLEN_MASK) + 1) & UHCI_TD_ACTLEN_MASK;
}

static int uhci_control(struct UsbController* hc, struct UsbDevice* dev,
                        const struct UsbSetupPacket* setup, void* data) {
    struct UhciState* st = (struct UhciState*)hc->priv;

    uint32_t length = setup->length;
    bool in = (setup->request_type & USB_DIR_IN) != 0;
    uint32_t mps = dev->max_packet0 ? dev->max_packet0 : 8;
    if (mps > 64) mps = 64;

    uint32_t data_tds = (length + mps - 1) / mps;
    uint32_t td_count = data_tds + 2;
    if (td_count > UHCI_MAX_TDS || length > USB_CONTROL_MAX)
        return USB_ERR_PARAM;

    memcpy(st->work + UHCI_SETUP_OFFSET, setup, sizeof(*setup));
    if (!in && length > 0)
        memcpy(st->data, data, length);

    uint32_t address = (uint32_t)(dev->address & 0x7F) << 8;
    uint32_t status  = UHCI_TD_ACTIVE | UHCI_TD_CERR_3 |
                       (dev->speed == USB_SPEED_LOW ? UHCI_TD_LS : 0);

    // Setup stage: DATA0
    struct UhciTd* td = uhci_td(st, 0);
    td->status = status;
    td->token  = UHCI_PID_SETUP | address | ((uint32_t)(sizeof(*setup) - 1) << 21);
    td->buffer = st->work_phys + UHCI_SETUP_OFFSET;

    // Data stage: DATA1, DATA0, ... An IN that comes back short stops the
    // queue (SPD) so the leftover TDs can be skipped.
    uint32_t offset = 0;
    for (uint32_t i = 0; i < data_tds; i++) {
        uint32_t chunk = length - offset < mps ? length - offset : mps;
        td = uhci_td(st, 1 + i);
        td->status = status | (in ? UHCI_TD_SPD : 0);
        td->token  = (in ? UHCI_PID_IN : UHCI_PID_OUT) | address |
                     ((i & 1) ? 0 : UHCI_TOKEN_TOGGLE) | ((chunk - 1) << 21);
        td->buffer = st->data_phys + offset;
        offset += chunk;
    }

    // Status stage: DATA1, zero length, opposite direction of the data
    uint32_t status_index = td_count - 1;
    td = uhci_td(st, status_index);
    td->status = status;
    td->token  = ((in && length > 0) ? UHCI_PID_OUT : UHCI_PID_IN) | address |
                 UHCI_TOKEN_TOGGLE | (0x7FFU << 21);
    td->buffer = 0;

    for (uint32_t i = 0; i < td_count; i++) {
        uhci_td(st, i)->link = (i + 1 < td_count)
            ? (uhci_td_phys(st, i + 1) | UHCI_LINK_DEPTH)
            : UHCI_LINK_TERMINATE;
    }

    struct UhciQh* qh = uhci_qh(st);
    usb_mmio_barrier();
    qh->element = uhci_td_phys(st, 0);

    int result = USB_ERR_TIMEOUT;
    uint64_t start = usb_time_ms();
    while (usb_time_ms() - start < UHCI_TIMEOUT_MS) {
        usb_mmio_barrier();

        // An error retires its TD and leaves the rest of the chain queued
        bool failed = false;
        for (uint32_t i = 0; i < td_count; i++) {
            uint32_t s = uhci_td(st, i)->status;
            if (!(s & UHCI_TD_ACTIVE) && (s & UHCI_TD_ERRORS)) {
                result = (s & UHCI_TD_STALLED) ? USB_ERR_STALL :
                         (s & UHCI_TD_CRC_TIMEOUT) ? USB_ERR_TIMEOUT : USB_ERR_IO;
                failed = true;
                break;
            }
        }
        if (failed) break;

        if (!(uhci_td(st, status_index)->status & UHCI_TD_ACTIVE)) {
            result = 0;
            break;
        }

        // Short IN packet: the controller stopped on a retired data TD.
        // Move the queue straight to the status stage.
        if (in) {
            for (uint32_t i = 1; i < status_index; i++) {
                uint32_t s = uhci_td(st, i)->status;
                if (s & UHCI_TD_ACTIVE) break;
                uint32_t max = ((uhci_td(st, i)->token >> 21) + 1) & 0x7FF;
                if (uhci_actual_length(s) < max && (uhci_td(st, i + 1)->status & UHCI_TD_ACTIVE)) {
                    qh->element = uhci_td_phys(st, status_index);
                    break;
                }
            }
        }
    }

    // Unhook the chain and give the controller a frame to let go of it
    qh->element = UHCI_LINK_TERMINATE;
    usb_mmio_barrier();
    usb_delay_ms(1);

    if (result < 0)
        return result;

    uint32_t moved = 0;
    for (uint32_t i = 1; i < status_index; i++) {
        uint32_t s = uhci_td(st, i)->status;
        if (s & UHCI_TD_ACTIVE) break;
        moved += uhci_actual_length(s);
    }
    if (moved > length) moved = length;

    if (in && moved > 0)
        memcpy(data, st->data, moved);
    return (int)moved;
}

const struct UsbHcdOps uhci_ops = {
    .name               = "uhci",
    .start              = uhci_start,
    .port_reset         = uhci_port_reset,
    .address_device     = usb_address_device_generic,
    .update_max_packet0 = NULL,
    .control            = uhci_control,
    .hub_configured     = NULL,
    .release_device     = NULL,
};
