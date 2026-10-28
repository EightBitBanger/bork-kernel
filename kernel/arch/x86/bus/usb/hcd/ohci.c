#include <kernel/arch/x86/bus/pci/pci.h>
#include <kernel/arch/x86/bus/usb/usb.h>
#include <kernel/arch/x86/bus/usb/usb_internal.h>
#include <kernel/arch/x86/virtual/vmm.h>

#include <kernel/malloc.h>
#include <kernel/console/print.h>
#include <kernel/util/string.h>

//
// OHCI (Open Host Controller Interface, USB 1.1)
//
// The USB 1.1 controller of AMD, nVidia, VIA (some), SiS and ALi chipsets,
// usually sitting next to an EHCI as its companion. One endpoint descriptor
// is kept on the control list. A control transfer hangs a chain of transfer
// descriptors from it and is polled until the chain retires. Low and full
// speed only. Interrupt IN endpoints (keyboards, mice) get an ED each on
// the periodic list, visited every frame. Bulk endpoints (storage) get an
// ED each on the bulk list, and a transfer is polled like a control one.
//

// Operational registers (BAR0, memory)
#define OHCI_REVISION           0x00
#define OHCI_CONTROL            0x04
#define OHCI_CMDSTATUS          0x08
#define OHCI_INTRSTATUS         0x0C
#define OHCI_INTRENABLE         0x10
#define OHCI_INTRDISABLE        0x14
#define OHCI_HCCA               0x18
#define OHCI_CONTROLHEAD        0x20
#define OHCI_CONTROLCURRENT     0x24
#define OHCI_BULKHEAD           0x28
#define OHCI_BULKCURRENT        0x2C
#define OHCI_FMINTERVAL         0x34
#define OHCI_PERIODICSTART      0x40
#define OHCI_LSTHRESHOLD        0x44
#define OHCI_RHDESCA            0x48
#define OHCI_RHDESCB            0x4C
#define OHCI_RHSTATUS           0x50
#define OHCI_RHPORTSTATUS       0x54

#define OHCI_CTRL_CBSR_4_1      0x00000003U     // 4 control EDs per bulk ED
#define OHCI_CTRL_PLE           0x00000004U     // Periodic list enable
#define OHCI_CTRL_BLE           0x00000020U     // Bulk list enable
#define OHCI_CTRL_CLE           0x00000010U     // Control list enable
#define OHCI_CTRL_HCFS_MASK     0x000000C0U
#define OHCI_CTRL_HCFS_RESET    0x00000000U
#define OHCI_CTRL_HCFS_RESUME   0x00000040U
#define OHCI_CTRL_HCFS_OPER     0x00000080U
#define OHCI_CTRL_IR            0x00000100U     // Interrupts routed to SMI (BIOS owns it)
#define OHCI_CTRL_RWC           0x00000200U     // Remote wakeup connected (kept as found)

#define OHCI_CMD_HCR            0x00000001U     // Controller reset
#define OHCI_CMD_CLF            0x00000002U     // Control list filled
#define OHCI_CMD_BLF            0x00000004U     // Bulk list filled
#define OHCI_CMD_OCR            0x00000008U     // Ownership change request

#define OHCI_INTR_ALL           0xC000007FU     // Every source plus MIE

#define OHCI_FMINTERVAL_FI      0x2EDF          // 11999: 1 ms frames
#define OHCI_FMINTERVAL_FIT     0x80000000U
#define OHCI_LSTHRESHOLD_VALUE  0x0628

#define OHCI_RHA_NDP_MASK       0x000000FFU
#define OHCI_RHA_PSM            0x00000100U     // Per-port power switching
#define OHCI_RHA_NPS            0x00000200U     // No power switching (always on)
#define OHCI_RHA_POTPGT_SHIFT   24              // Power on to power good, 2 ms units

#define OHCI_RHS_LPSC           0x00010000U     // Write: global power on

// Port status: read bits
#define OHCI_PORT_CCS           0x00000001U     // Device connected
#define OHCI_PORT_PES           0x00000002U     // Enabled
#define OHCI_PORT_PRS           0x00000010U     // Reset in progress
#define OHCI_PORT_LSDA          0x00000200U     // Low speed device attached
#define OHCI_PORT_CSC           0x00010000U     // Connect status change
#define OHCI_PORT_CHANGES       0x001F0000U     // CSC, PESC, PSSC, OCIC, PRSC (RWC)
// Port status: write commands
#define OHCI_PORT_SET_ENABLE    0x00000002U
#define OHCI_PORT_SET_RESET     0x00000010U
#define OHCI_PORT_SET_POWER     0x00000100U

#define OHCI_MMIO_SIZE          0x100
#define OHCI_MAX_PORTS          15
#define OHCI_TIMEOUT_MS         1000

// Endpoint descriptor
#define OHCI_ED_LOW_SPEED       (1U << 13)
#define OHCI_ED_SKIP            (1U << 14)
#define OHCI_ED_MPS_SHIFT       16
#define OHCI_ED_HALTED          0x00000001U     // In head_td
#define OHCI_ED_CARRY           0x00000002U     // In head_td: toggle carry
#define OHCI_ED_EN_SHIFT        7
#define OHCI_ED_PTR_MASK        0xFFFFFFF0U

// General transfer descriptor
#define OHCI_TD_ROUNDING        (1U << 18)      // Short packet is not an error
#define OHCI_TD_DP_SETUP        (0U << 19)
#define OHCI_TD_DP_OUT          (1U << 19)
#define OHCI_TD_DP_IN           (2U << 19)
#define OHCI_TD_NO_INTR         (7U << 21)
#define OHCI_TD_DATA0           (2U << 24)      // Toggle from the TD, DATA0
#define OHCI_TD_DATA1           (3U << 24)      // Toggle from the TD, DATA1
#define OHCI_TD_TOGGLE_ED       (0U << 24)      // Toggle from the ED's carry
#define OHCI_TD_CC_SHIFT        28
#define OHCI_TD_CC_NOT_ACCESSED (0xFU << OHCI_TD_CC_SHIFT)

#define OHCI_CC_NO_ERROR        0x0
#define OHCI_CC_STALL           0x4
#define OHCI_CC_NOT_RESPONDING  0x5
#define OHCI_CC_DATA_UNDERRUN   0x9

// Work page layout: endpoint descriptor, setup packet, then the TDs
// (setup, data, status and the empty tail the ED's TailP points at)
#define OHCI_ED_OFFSET          0
#define OHCI_SETUP_OFFSET       16
#define OHCI_TD_OFFSET          32
#define OHCI_TD_SETUP           0
#define OHCI_TD_DATA            1
#define OHCI_TD_STATUS          2
#define OHCI_TD_TAIL            3

struct __attribute__((packed, aligned(16))) OhciEd {
    volatile uint32_t control;
    volatile uint32_t tail_td;
    volatile uint32_t head_td;      // Low bits: halted, toggle carry
    volatile uint32_t next_ed;
};

struct __attribute__((packed, aligned(16))) OhciTd {
    volatile uint32_t control;
    volatile uint32_t buffer;       // Current buffer pointer, 0 when all moved
    volatile uint32_t next_td;
    volatile uint32_t buffer_end;   // Last byte of the buffer
};

// Interrupt pipe page: ED, two TDs taking turns as the queued one and the
// empty tail, and a buffer for each
#define OHCI_PIPE_ED            0
#define OHCI_PIPE_TD            16
#define OHCI_PIPE_BUFFER        64

#define OHCI_HCCA_INT_TABLE     32      // Entries of the HCCA interrupt table
#define OHCI_MAX_PIPES          16

struct OhciPipe {
    uint8_t*  page;
    uint32_t  phys;
    uint32_t  queued;           // TD (0 or 1) handed to the controller
};

struct OhciState {
    volatile uint8_t* regs;
    uint32_t  int_head;         // First ED of the periodic list, 0 if none
    struct OhciPipe* pipes[OHCI_MAX_PIPES];    // In periodic list order
    uint32_t  pipe_count;
    struct OhciPipe* bulk[OHCI_MAX_PIPES];     // In bulk list order
    uint32_t  bulk_count;
    uint8_t*  hcca;             // Host controller communication area
    uint8_t*  work;             // ED, setup packet, TDs
    uint32_t  work_phys;
    uint8_t*  data;             // Data stage bounce buffer (USB_CONTROL_MAX)
    uint32_t  data_phys;
};

static inline uint32_t ohci_read(struct OhciState* st, uint32_t offset) {
    return *(volatile uint32_t*)(st->regs + offset);
}

static inline void ohci_write(struct OhciState* st, uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)(st->regs + offset) = value;
}

static inline struct OhciEd* ohci_ed(struct OhciState* st) {
    return (struct OhciEd*)(st->work + OHCI_ED_OFFSET);
}

static inline struct OhciTd* ohci_td(struct OhciState* st, uint32_t i) {
    return (struct OhciTd*)(st->work + OHCI_TD_OFFSET) + i;
}

static inline uint32_t ohci_td_phys(struct OhciState* st, uint32_t i) {
    return st->work_phys + OHCI_TD_OFFSET + i * (uint32_t)sizeof(struct OhciTd);
}

static inline uint32_t ohci_port_reg(uint8_t port) {
    return OHCI_RHPORTSTATUS + (uint32_t)(port - 1) * 4;
}

// Take it from the BIOS (OHCI 5.1.1.3.3). With IR set an SMM driver owns
// it and is asked to let go. Without it the BIOS may still have left the
// controller running; the reset that follows stops that.
static void ohci_bios_handoff(struct OhciState* st) {
    if (!(ohci_read(st, OHCI_CONTROL) & OHCI_CTRL_IR))
        return;

    ohci_write(st, OHCI_CMDSTATUS, OHCI_CMD_OCR);
    uint64_t start = usb_time_ms();
    while (ohci_read(st, OHCI_CONTROL) & OHCI_CTRL_IR) {
        if (usb_time_ms() - start > 500) {
            usb_print("OHCI: BIOS did not release the controller\n");
            return;
        }
    }
}

static void ohci_free_state(struct OhciState* st) {
    usb_dma_page_free(st->hcca);
    usb_dma_page_free(st->work);
    usb_dma_page_free(st->data);
    free(st);
}

static bool ohci_start(struct UsbController* hc) {
    const struct PCIDeviceInfo* pci = &hc->pci;
    pci_enable_command_bits(pci->bus, pci->dev, pci->func, PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER);

    volatile uint8_t* regs = (volatile uint8_t*)pci_map_bar(pci->bus, pci->dev, pci->func, 0, OHCI_MMIO_SIZE,
                                                            VM_PRESENT | VM_READWRITE | VM_PCD);
    if (regs == NULL) {
        usb_print("OHCI: registers not mappable\n");
        return false;
    }

    struct OhciState* st = (struct OhciState*)malloc(sizeof(struct OhciState));
    if (st == NULL) return false;
    memset(st, 0, sizeof(*st));
    st->regs = regs;

    uint32_t hcca_phys = 0;
    st->hcca = (uint8_t*)usb_dma_page(&hcca_phys);
    st->work = (uint8_t*)usb_dma_page(&st->work_phys);
    st->data = (uint8_t*)usb_dma_page(&st->data_phys);
    if (st->hcca == NULL || st->work == NULL || st->data == NULL) {
        ohci_free_state(st);
        usb_print("OHCI: out of memory\n");
        return false;
    }

    if ((ohci_read(st, OHCI_REVISION) & 0xFF) != 0x10) {
        ohci_free_state(st);
        usb_print("OHCI: unknown controller revision\n");
        return false;
    }

    ohci_bios_handoff(st);
    ohci_write(st, OHCI_INTRDISABLE, OHCI_INTR_ALL);

    // Signal a bus reset downstream, then reset the controller. The reset
    // leaves it suspended and it has to be made operational within 2 ms,
    // so everything it needs is prepared before.
    uint32_t rwc = ohci_read(st, OHCI_CONTROL) & OHCI_CTRL_RWC;
    ohci_write(st, OHCI_CONTROL, rwc | OHCI_CTRL_HCFS_RESET);
    usb_delay_ms(50);

    // The control list holds one ED, skipped while no transfer runs. Its
    // queue is empty: head and tail both point at the tail TD.
    struct OhciEd* ed = ohci_ed(st);
    ed->control = OHCI_ED_SKIP | (8U << OHCI_ED_MPS_SHIFT);
    ed->tail_td = ohci_td_phys(st, OHCI_TD_TAIL);
    ed->head_td = ohci_td_phys(st, OHCI_TD_TAIL);
    ed->next_ed = 0;
    usb_mmio_barrier();

    uint32_t fsmps = ((OHCI_FMINTERVAL_FI - 210) * 6) / 7;  // Largest full-speed packet
    uint32_t fit = (ohci_read(st, OHCI_FMINTERVAL) & OHCI_FMINTERVAL_FIT) ^ OHCI_FMINTERVAL_FIT;

    ohci_write(st, OHCI_CMDSTATUS, OHCI_CMD_HCR);
    uint64_t start = usb_time_ms();
    while (ohci_read(st, OHCI_CMDSTATUS) & OHCI_CMD_HCR) {
        if (usb_time_ms() - start > 50) {
            ohci_free_state(st);
            usb_print("OHCI: controller reset timed out\n");
            return false;
        }
    }

    ohci_write(st, OHCI_HCCA, hcca_phys);
    ohci_write(st, OHCI_CONTROLHEAD, st->work_phys + OHCI_ED_OFFSET);
    ohci_write(st, OHCI_CONTROLCURRENT, 0);
    ohci_write(st, OHCI_BULKHEAD, 0);
    ohci_write(st, OHCI_BULKCURRENT, 0);
    ohci_write(st, OHCI_INTRDISABLE, OHCI_INTR_ALL);
    ohci_write(st, OHCI_INTRSTATUS, OHCI_INTR_ALL);
    ohci_write(st, OHCI_CONTROL, rwc | OHCI_CTRL_HCFS_OPER | OHCI_CTRL_CLE | OHCI_CTRL_CBSR_4_1);
    ohci_write(st, OHCI_FMINTERVAL, fit | (fsmps << 16) | OHCI_FMINTERVAL_FI);
    ohci_write(st, OHCI_PERIODICSTART, (OHCI_FMINTERVAL_FI * 9) / 10);
    ohci_write(st, OHCI_LSTHRESHOLD, OHCI_LSTHRESHOLD_VALUE);
    usb_delay_ms(2);

    if ((ohci_read(st, OHCI_CONTROL) & OHCI_CTRL_HCFS_MASK) != OHCI_CTRL_HCFS_OPER) {
        ohci_free_state(st);
        usb_print("OHCI: controller did not start\n");
        return false;
    }

    // Root hub power. Globally switched ports come on with LPSC; per-port
    // switched ones need their own SetPortPower. Doing both covers either.
    uint32_t desc_a = ohci_read(st, OHCI_RHDESCA);
    uint8_t ports = (uint8_t)(desc_a & OHCI_RHA_NDP_MASK);
    if (ports > OHCI_MAX_PORTS) ports = OHCI_MAX_PORTS;

    if (!(desc_a & OHCI_RHA_NPS)) {
        ohci_write(st, OHCI_RHSTATUS, OHCI_RHS_LPSC);
        if (desc_a & OHCI_RHA_PSM) {
            for (uint8_t port = 1; port <= ports; port++)
                ohci_write(st, ohci_port_reg(port), OHCI_PORT_SET_POWER);
        }
    }

    // Power on to power good, then 100 ms for devices to connect and
    // settle (USB 2.0 7.1.7.3)
    uint32_t potpgt_ms = ((desc_a >> OHCI_RHA_POTPGT_SHIFT) & 0xFF) * 2;
    if (potpgt_ms < 20) potpgt_ms = 20;
    usb_delay_ms(potpgt_ms + 100);

    hc->port_count = ports;
    hc->priv = st;
    return true;
}

static uint8_t ohci_port_reset(struct UsbController* hc, uint8_t port) {
    struct OhciState* st = (struct OhciState*)hc->priv;
    uint32_t reg = ohci_port_reg(port);

    if (!(ohci_read(st, reg) & OHCI_PORT_CCS))
        return 0;

    // The controller times the reset itself (10 ms) and enables the port
    // when it ends
    ohci_write(st, reg, OHCI_PORT_CHANGES);
    ohci_write(st, reg, OHCI_PORT_SET_RESET);

    uint64_t start = usb_time_ms();
    while (ohci_read(st, reg) & OHCI_PORT_PRS) {
        if (usb_time_ms() - start > 100)
            return 0;
    }
    ohci_write(st, reg, OHCI_PORT_CHANGES);

    uint32_t v = ohci_read(st, reg);
    if (!(v & OHCI_PORT_CCS))
        return 0;
    if (!(v & OHCI_PORT_PES)) {
        ohci_write(st, reg, OHCI_PORT_SET_ENABLE);
        usb_delay_ms(2);
        v = ohci_read(st, reg);
        if (!(v & OHCI_PORT_PES))
            return 0;
    }

    usb_delay_ms(10);   // Reset recovery
    return (v & OHCI_PORT_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
}

static inline uint32_t ohci_td_cc(const struct OhciTd* td) {
    return td->control >> OHCI_TD_CC_SHIFT;
}

static int ohci_cc_error(uint32_t cc) {
    switch (cc) {
        case OHCI_CC_STALL:          return USB_ERR_STALL;
        case OHCI_CC_NOT_RESPONDING: return USB_ERR_TIMEOUT;
        default:                     return USB_ERR_IO;
    }
}

static void ohci_fill_td(struct OhciTd* td, uint32_t control, uint32_t buffer, uint32_t length, uint32_t next) {
    td->control    = control | OHCI_TD_NO_INTR | OHCI_TD_CC_NOT_ACCESSED;
    td->buffer     = length ? buffer : 0;
    td->buffer_end = length ? buffer + length - 1 : 0;
    td->next_td    = next;
}

static int ohci_control(struct UsbController* hc, struct UsbDevice* dev,
                        const struct UsbSetupPacket* setup, void* data) {
    struct OhciState* st = (struct OhciState*)hc->priv;

    uint32_t length = setup->length;
    bool in = (setup->request_type & USB_DIR_IN) != 0;
    uint32_t mps = dev->max_packet0 ? dev->max_packet0 : 8;
    if (mps > 64) mps = 64;

    // The whole data stage fits one TD: the bounce buffer is one page
    if (length > USB_CONTROL_MAX)
        return USB_ERR_PARAM;

    memcpy(st->work + OHCI_SETUP_OFFSET, setup, sizeof(*setup));
    if (!in && length > 0)
        memcpy(st->data, data, length);

    // Setup (DATA0), data (DATA1 first, the controller toggles from there),
    // status (DATA1, opposite direction), then the empty tail
    uint32_t status_td = length > 0 ? OHCI_TD_STATUS : OHCI_TD_DATA;
    ohci_fill_td(ohci_td(st, OHCI_TD_SETUP), OHCI_TD_DP_SETUP | OHCI_TD_DATA0,
                 st->work_phys + OHCI_SETUP_OFFSET, sizeof(*setup), ohci_td_phys(st, OHCI_TD_DATA));
    if (length > 0) {
        ohci_fill_td(ohci_td(st, OHCI_TD_DATA),
                     (in ? OHCI_TD_DP_IN | OHCI_TD_ROUNDING : OHCI_TD_DP_OUT) | OHCI_TD_DATA1,
                     st->data_phys, length, ohci_td_phys(st, OHCI_TD_STATUS));
    }
    ohci_fill_td(ohci_td(st, status_td),
                 ((in && length > 0) ? OHCI_TD_DP_OUT : OHCI_TD_DP_IN) | OHCI_TD_DATA1,
                 0, 0, ohci_td_phys(st, OHCI_TD_TAIL));
    memset(ohci_td(st, OHCI_TD_TAIL), 0, sizeof(struct OhciTd));

    // Point the ED at the device and hand it the chain. Writing head_td
    // also clears a halt left by an earlier stall.
    struct OhciEd* ed = ohci_ed(st);
    ed->control = (uint32_t)(dev->address & 0x7F) | (mps << OHCI_ED_MPS_SHIFT) |
                  (dev->speed == USB_SPEED_LOW ? OHCI_ED_LOW_SPEED : 0) | OHCI_ED_SKIP;
    ed->tail_td = ohci_td_phys(st, OHCI_TD_TAIL);
    ed->head_td = ohci_td_phys(st, OHCI_TD_SETUP);
    usb_mmio_barrier();
    ed->control &= ~OHCI_ED_SKIP;
    usb_mmio_barrier();
    ohci_write(st, OHCI_CMDSTATUS, OHCI_CMD_CLF);

    // Done when the queue drains to the tail, or the ED halts on an error
    int result = USB_ERR_TIMEOUT;
    uint64_t start = usb_time_ms();
    while (usb_time_ms() - start < OHCI_TIMEOUT_MS) {
        usb_mmio_barrier();
        uint32_t head = ed->head_td;
        if (head & OHCI_ED_HALTED) {
            result = USB_ERR_IO;
            for (uint32_t i = OHCI_TD_SETUP; i <= status_td; i++) {
                uint32_t cc = ohci_td_cc(ohci_td(st, i));
                if (cc != OHCI_CC_NO_ERROR && cc < 0xE) {
                    result = ohci_cc_error(cc);
                    break;
                }
            }
            break;
        }
        if ((head & OHCI_ED_PTR_MASK) == ed->tail_td) {
            result = 0;
            break;
        }
    }

    // Skip the ED again, give the controller a frame to let go of it, and
    // leave the queue empty for the next transfer
    ed->control |= OHCI_ED_SKIP;
    usb_mmio_barrier();
    usb_delay_ms(2);
    ed->head_td = ohci_td_phys(st, OHCI_TD_TAIL);
    usb_mmio_barrier();

    if (result < 0)
        return result;

    // An IN shorter than asked leaves the buffer pointer on the first byte
    // not written; 0 means every byte moved
    uint32_t moved = 0;
    if (length > 0) {
        uint32_t cbp = ohci_td(st, OHCI_TD_DATA)->buffer;
        moved = cbp == 0 ? length : cbp - st->data_phys;
        if (moved > length) moved = length;
    }

    if (in && moved > 0)
        memcpy(data, st->data, moved);
    return (int)moved;
}

//
// Interrupt IN endpoints
//

static inline struct OhciEd* ohci_pipe_ed(struct OhciPipe* op) {
    return (struct OhciEd*)(op->page + OHCI_PIPE_ED);
}

static inline struct OhciTd* ohci_pipe_td(struct OhciPipe* op, uint32_t i) {
    return (struct OhciTd*)(op->page + OHCI_PIPE_TD) + i;
}

static inline uint32_t ohci_pipe_td_phys(struct OhciPipe* op, uint32_t i) {
    return op->phys + OHCI_PIPE_TD + i * (uint32_t)sizeof(struct OhciTd);
}

static inline uint32_t ohci_pipe_buffer_phys(struct OhciPipe* op, uint32_t i) {
    return op->phys + OHCI_PIPE_BUFFER + i * USB_INT_MAX_LENGTH;
}

// Queue TD `i` in front of the other (empty) TD, which becomes the tail.
// Moving the ED's tail pointer is what hands it to the controller.
static void ohci_pipe_queue(struct OhciPipe* op, uint32_t i, uint16_t length) {
    uint32_t tail = i ^ 1;
    memset(ohci_pipe_td(op, tail), 0, sizeof(struct OhciTd));
    ohci_fill_td(ohci_pipe_td(op, i), OHCI_TD_DP_IN | OHCI_TD_ROUNDING | OHCI_TD_TOGGLE_ED,
                 ohci_pipe_buffer_phys(op, i), length, ohci_pipe_td_phys(op, tail));
    op->queued = i;
    usb_mmio_barrier();
    ohci_pipe_ed(op)->tail_td = ohci_pipe_td_phys(op, tail);
}

static bool ohci_int_open(struct UsbController* hc, struct UsbIntPipe* pipe) {
    struct OhciState* st = (struct OhciState*)hc->priv;
    struct UsbDevice* dev = pipe->dev;

    if (st->pipe_count >= OHCI_MAX_PIPES) return false;
    struct OhciPipe* op = (struct OhciPipe*)malloc(sizeof(struct OhciPipe));
    if (op == NULL) return false;
    op->page = (uint8_t*)usb_dma_page(&op->phys);
    if (op->page == NULL) {
        free(op);
        return false;
    }

    uint32_t mps = pipe->max_packet ? pipe->max_packet : 8;
    if (mps > 64) mps = 64;

    // Head and tail on TD 0: an empty queue until the first TD is queued
    struct OhciEd* ed = ohci_pipe_ed(op);
    ed->control = (uint32_t)(dev->address & 0x7F) | ((uint32_t)(pipe->endpoint & 0x0F) << OHCI_ED_EN_SHIFT) |
                  (mps << OHCI_ED_MPS_SHIFT) | (dev->speed == USB_SPEED_LOW ? OHCI_ED_LOW_SPEED : 0);
    ed->tail_td = ohci_pipe_td_phys(op, 0);
    ed->head_td = ohci_pipe_td_phys(op, 0);
    ed->next_ed = st->int_head;
    ohci_pipe_queue(op, 0, pipe->length);

    // Every slot of the interrupt table starts at the newest ED, so each
    // endpoint is visited every frame
    uint32_t ed_phys = op->phys + OHCI_PIPE_ED;
    usb_mmio_barrier();
    uint32_t* table = (uint32_t*)st->hcca;
    for (uint32_t i = 0; i < OHCI_HCCA_INT_TABLE; i++)
        table[i] = ed_phys;
    st->int_head = ed_phys;
    for (uint32_t i = st->pipe_count; i > 0; i--)
        st->pipes[i] = st->pipes[i - 1];
    st->pipes[0] = op;
    st->pipe_count++;
    usb_mmio_barrier();
    ohci_write(st, OHCI_CONTROL, ohci_read(st, OHCI_CONTROL) | OHCI_CTRL_PLE);

    pipe->hcd = op;
    return true;
}

static int ohci_int_poll(struct UsbController* hc, struct UsbIntPipe* pipe, void* buf) {
    (void)hc;
    struct OhciPipe* op = (struct OhciPipe*)pipe->hcd;
    if (op == NULL) return USB_ERR_PARAM;

    struct OhciEd* ed = ohci_pipe_ed(op);
    usb_mmio_barrier();
    uint32_t head = ed->head_td;
    bool halted = (head & OHCI_ED_HALTED) != 0;
    if (!halted && (head & OHCI_ED_PTR_MASK) != ed->tail_td)
        return 0;   // Still queued: the device keeps NAKing until it has news

    uint32_t done = op->queued;
    struct OhciTd* td = ohci_pipe_td(op, done);
    uint32_t cc = ohci_td_cc(td);

    int result;
    if (cc == OHCI_CC_NO_ERROR) {
        uint32_t start = ohci_pipe_buffer_phys(op, done);
        uint32_t moved = td->buffer == 0 ? pipe->length : td->buffer - start;
        if (moved > pipe->length) moved = pipe->length;
        memcpy(buf, op->page + OHCI_PIPE_BUFFER + done * USB_INT_MAX_LENGTH, moved);
        result = (int)moved;
    } else {
        result = ohci_cc_error(cc);
    }

    ohci_pipe_queue(op, done ^ 1, pipe->length);

    // A halt (and the toggle carry) is cleared by rewriting the head; the
    // queue starts over at the TD just queued
    if (halted) {
        usb_mmio_barrier();
        ed->head_td = ohci_pipe_td_phys(op, done ^ 1);
    }
    return result;
}

static void ohci_int_close(struct UsbController* hc, struct UsbIntPipe* pipe) {
    struct OhciState* st = (struct OhciState*)hc->priv;
    struct OhciPipe* op = (struct OhciPipe*)pipe->hcd;
    if (op == NULL) return;

    uint32_t index = 0;
    while (index < st->pipe_count && st->pipes[index] != op)
        index++;
    if (index == st->pipe_count) return;

    // Skip it, then unhook it from whatever leads to it: the interrupt
    // table when it is first, the ED before it otherwise
    struct OhciEd* ed = ohci_pipe_ed(op);
    ed->control |= OHCI_ED_SKIP;
    uint32_t next = ed->next_ed;
    usb_mmio_barrier();
    if (index == 0) {
        uint32_t* table = (uint32_t*)st->hcca;
        for (uint32_t i = 0; i < OHCI_HCCA_INT_TABLE; i++)
            table[i] = next;
        st->int_head = next;
    } else {
        ohci_pipe_ed(st->pipes[index - 1])->next_ed = next;
    }
    for (uint32_t i = index; i + 1 < st->pipe_count; i++)
        st->pipes[i] = st->pipes[i + 1];
    st->pipe_count--;

    // Two frames for the controller to stop looking at it
    usb_mmio_barrier();
    usb_delay_ms(2);
    usb_dma_page_free(op->page);
    free(op);
    pipe->hcd = NULL;
}

static bool ohci_port_changed(struct UsbController* hc, uint8_t port, bool* connected) {
    struct OhciState* st = (struct OhciState*)hc->priv;
    uint32_t reg = ohci_port_reg(port);
    uint32_t v = ohci_read(st, reg);
    *connected = (v & OHCI_PORT_CCS) != 0;
    if (!(v & OHCI_PORT_CSC))
        return false;
    ohci_write(st, reg, OHCI_PORT_CHANGES);
    return true;
}

//
// Bulk endpoints
//
// Same page layout as an interrupt pipe (ED, two TDs taking turns); the
// data goes through the controller's bounce buffer. The ED's toggle carry
// keeps the data toggle from one transfer to the next.
//

// Bulk list changes are made with the list switched off
static void ohci_bulk_list_pause(struct OhciState* st, bool pause) {
    uint32_t control = ohci_read(st, OHCI_CONTROL);
    ohci_write(st, OHCI_CONTROL, pause ? (control & ~OHCI_CTRL_BLE) : (control | OHCI_CTRL_BLE));
    if (pause)
        usb_delay_ms(2);
}

static bool ohci_bulk_open(struct UsbController* hc, struct UsbBulkPipe* pipe) {
    struct OhciState* st = (struct OhciState*)hc->priv;
    struct UsbDevice* dev = pipe->dev;
    if (st->bulk_count >= OHCI_MAX_PIPES) return false;

    struct OhciPipe* op = (struct OhciPipe*)malloc(sizeof(struct OhciPipe));
    if (op == NULL) return false;
    op->page = (uint8_t*)usb_dma_page(&op->phys);
    if (op->page == NULL) {
        free(op);
        return false;
    }
    op->queued = 0;     // Here: the TD the queue is empty at

    uint32_t mps = pipe->max_packet ? pipe->max_packet : 64;
    if (mps > 64) mps = 64;

    struct OhciEd* ed = ohci_pipe_ed(op);
    ed->control = (uint32_t)(dev->address & 0x7F) | ((uint32_t)(pipe->endpoint & 0x0F) << OHCI_ED_EN_SHIFT) |
                  (mps << OHCI_ED_MPS_SHIFT) | (dev->speed == USB_SPEED_LOW ? OHCI_ED_LOW_SPEED : 0);
    ed->tail_td = ohci_pipe_td_phys(op, 0);
    ed->head_td = ohci_pipe_td_phys(op, 0);

    ohci_bulk_list_pause(st, true);
    ed->next_ed = ohci_read(st, OHCI_BULKHEAD);
    usb_mmio_barrier();
    ohci_write(st, OHCI_BULKHEAD, op->phys + OHCI_PIPE_ED);
    ohci_write(st, OHCI_BULKCURRENT, 0);
    for (uint32_t i = st->bulk_count; i > 0; i--)
        st->bulk[i] = st->bulk[i - 1];
    st->bulk[0] = op;
    st->bulk_count++;
    ohci_bulk_list_pause(st, false);

    pipe->hcd = op;
    return true;
}

// Empty the ED's queue (after a timeout or a halt), keeping or clearing
// the toggle carry
static void ohci_bulk_flush(struct OhciPipe* op, bool keep_toggle) {
    struct OhciEd* ed = ohci_pipe_ed(op);
    ed->control |= OHCI_ED_SKIP;
    usb_mmio_barrier();
    usb_delay_ms(2);

    uint32_t carry = keep_toggle ? (ed->head_td & OHCI_ED_CARRY) : 0;
    ed->tail_td = ohci_pipe_td_phys(op, op->queued);
    ed->head_td = ohci_pipe_td_phys(op, op->queued) | carry;
    usb_mmio_barrier();
    ed->control &= ~OHCI_ED_SKIP;
}

static int ohci_bulk_transfer(struct UsbController* hc, struct UsbBulkPipe* pipe, void* data,
                              uint32_t length, uint32_t timeout_ms) {
    struct OhciState* st = (struct OhciState*)hc->priv;
    struct OhciPipe* op = (struct OhciPipe*)pipe->hcd;
    if (op == NULL || length > USB_BULK_MAX || length > USB_CONTROL_MAX) return USB_ERR_PARAM;

    if (!pipe->in && length > 0)
        memcpy(st->data, data, length);

    // The empty TD the queue stops at gets the transfer; the other one
    // becomes the new empty tail
    uint32_t t = op->queued;
    uint32_t tail = t ^ 1;
    memset(ohci_pipe_td(op, tail), 0, sizeof(struct OhciTd));
    ohci_fill_td(ohci_pipe_td(op, t),
                 (pipe->in ? OHCI_TD_DP_IN | OHCI_TD_ROUNDING : OHCI_TD_DP_OUT) | OHCI_TD_TOGGLE_ED,
                 st->data_phys, length, ohci_pipe_td_phys(op, tail));
    op->queued = tail;

    struct OhciEd* ed = ohci_pipe_ed(op);
    usb_mmio_barrier();
    ed->tail_td = ohci_pipe_td_phys(op, tail);
    ohci_write(st, OHCI_CMDSTATUS, OHCI_CMD_BLF);

    int result = USB_ERR_TIMEOUT;
    uint64_t start = usb_time_ms();
    while (usb_time_ms() - start < timeout_ms) {
        usb_mmio_barrier();
        uint32_t head = ed->head_td;
        if (head & OHCI_ED_HALTED) {
            uint32_t cc = ohci_td_cc(ohci_pipe_td(op, t));
            result = cc == OHCI_CC_NO_ERROR ? USB_ERR_IO : ohci_cc_error(cc);
            break;
        }
        if ((head & OHCI_ED_PTR_MASK) == ed->tail_td) {
            result = 0;
            break;
        }
    }

    if (result < 0) {
        // A stall is cleared by bulk_reset; anything else keeps the toggle
        ohci_bulk_flush(op, result != USB_ERR_STALL);
        return result;
    }

    uint32_t moved = 0;
    if (length > 0) {
        uint32_t cbp = ohci_pipe_td(op, t)->buffer;
        moved = cbp == 0 ? length : cbp - st->data_phys;
        if (moved > length) moved = length;
    }
    if (pipe->in && moved > 0)
        memcpy(data, st->data, moved);
    return (int)moved;
}

static void ohci_bulk_reset(struct UsbController* hc, struct UsbBulkPipe* pipe) {
    (void)hc;
    struct OhciPipe* op = (struct OhciPipe*)pipe->hcd;
    if (op != NULL)
        ohci_bulk_flush(op, false);
}

static void ohci_bulk_close(struct UsbController* hc, struct UsbBulkPipe* pipe) {
    struct OhciState* st = (struct OhciState*)hc->priv;
    struct OhciPipe* op = (struct OhciPipe*)pipe->hcd;
    if (op == NULL) return;

    uint32_t index = 0;
    while (index < st->bulk_count && st->bulk[index] != op)
        index++;
    if (index == st->bulk_count) return;

    ohci_bulk_list_pause(st, true);
    uint32_t next = ohci_pipe_ed(op)->next_ed;
    if (index == 0)
        ohci_write(st, OHCI_BULKHEAD, next);
    else
        ohci_pipe_ed(st->bulk[index - 1])->next_ed = next;
    ohci_write(st, OHCI_BULKCURRENT, 0);
    for (uint32_t i = index; i + 1 < st->bulk_count; i++)
        st->bulk[i] = st->bulk[i + 1];
    st->bulk_count--;
    ohci_bulk_list_pause(st, false);

    usb_dma_page_free(op->page);
    free(op);
    pipe->hcd = NULL;
}

const struct UsbHcdOps ohci_ops = {
    .name               = "ohci",
    .start              = ohci_start,
    .port_reset         = ohci_port_reset,
    .address_device     = usb_address_device_generic,
    .update_max_packet0 = NULL,
    .control            = ohci_control,
    .hub_configured     = NULL,
    .release_device     = NULL,
    .int_open           = ohci_int_open,
    .int_poll           = ohci_int_poll,
    .int_close          = ohci_int_close,
    .port_changed       = ohci_port_changed,
    .bulk_open          = ohci_bulk_open,
    .bulk_transfer      = ohci_bulk_transfer,
    .bulk_reset         = ohci_bulk_reset,
    .bulk_close         = ohci_bulk_close,
};
