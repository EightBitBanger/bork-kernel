#include <kernel/arch/x86/bus/pci/pci.h>
#include <kernel/arch/x86/bus/usb/usb.h>
#include <kernel/arch/x86/bus/usb/usb_internal.h>
#include <kernel/arch/x86/virtual/vmm.h>

#include <kernel/console/print.h>

//
// EHCI (USB 2.0) is not driven. Each EHCI port has a companion UHCI/OHCI
// port wired to the same connector, and while the EHCI CONFIGFLAG is 0 every
// port belongs to the companion. Firmware that set it leaves high-speed
// devices on EHCI, invisible to the companion, so it is taken from the
// firmware, halted, and CONFIGFLAG is cleared. High-speed devices then
// enumerate at full speed on the companion.
//
// Machines whose EHCI has no companions (rate-matching hubs, Intel 5-series
// and later) lose nothing: their USB ports were unreachable without an EHCI
// driver anyway.
//

#define EHCI_CAPLENGTH          0x00
#define EHCI_HCSPARAMS          0x04
#define EHCI_HCCPARAMS          0x08

#define EHCI_USBCMD             0x00
#define EHCI_USBSTS             0x04
#define EHCI_USBINTR            0x08
#define EHCI_CONFIGFLAG         0x40
#define EHCI_PORTSC             0x44

#define EHCI_PORT_RWC           ((1U << 1) | (1U << 3) | (1U << 5))     // Change bits
#define EHCI_PORT_OWNER         (1U << 13)                              // 1 = companion

#define EHCI_CMD_RUN            0x00000001U
#define EHCI_STS_HALTED         0x00001000U

// Extended capability in PCI config space (EECP)
#define EHCI_CAP_LEGACY         0x01
#define EHCI_LEGSUP_BIOS_OWNED  (1U << 16)
#define EHCI_LEGSUP_OS_OWNED    (1U << 24)

#define EHCI_MMIO_SIZE          0x100

static inline uint32_t ehci_read(volatile uint8_t* base, uint32_t offset) {
    return *(volatile uint32_t*)(base + offset);
}

static inline void ehci_write(volatile uint8_t* base, uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)(base + offset) = value;
}

// Ask the BIOS to let go (EHCI 5.1), then switch its SMIs off
static void ehci_bios_handoff(const struct PCIDeviceInfo* pci, uint8_t eecp) {
    uint8_t guard = 0;
    while (eecp >= 0x40 && guard++ < 16) {
        uint32_t cap = pci_config_read(pci->bus, pci->dev, pci->func, eecp);

        if ((cap & 0xFF) == EHCI_CAP_LEGACY) {
            if (cap & EHCI_LEGSUP_BIOS_OWNED) {
                pci_config_write8(pci->bus, pci->dev, pci->func, (uint8_t)(eecp + 3), 1);

                uint64_t start = usb_time_ms();
                while (pci_config_read(pci->bus, pci->dev, pci->func, eecp) & EHCI_LEGSUP_BIOS_OWNED) {
                    if (usb_time_ms() - start > 1000) {
                        usb_print("EHCI: BIOS did not release the controller\n");
                        break;
                    }
                }
            }
            // USBLEGCTLSTS: SMI enables off
            pci_config_write(pci->bus, pci->dev, pci->func, (uint8_t)(eecp + 4), 0);
            return;
        }
        eecp = (uint8_t)((cap >> 8) & 0xFF);
    }
}

void ehci_release_ports(const struct PCIDeviceInfo* pci) {
    pci_enable_command_bits(pci->bus, pci->dev, pci->func, PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER);

    volatile uint8_t* cap = (volatile uint8_t*)pci_map_bar(pci->bus, pci->dev, pci->func, 0, EHCI_MMIO_SIZE,
                                                           VM_PRESENT | VM_READWRITE | VM_PCD);
    if (cap == NULL)
        return;

    uint8_t eecp = (uint8_t)((ehci_read(cap, EHCI_HCCPARAMS) >> 8) & 0xFF);
    ehci_bios_handoff(pci, eecp);

    volatile uint8_t* op = cap + *(volatile uint8_t*)(cap + EHCI_CAPLENGTH);

    // Halt it (no reset: that would power the ports off on some chipsets)
    ehci_write(op, EHCI_USBINTR, 0);
    ehci_write(op, EHCI_USBCMD, ehci_read(op, EHCI_USBCMD) & ~EHCI_CMD_RUN);
    uint64_t start = usb_time_ms();
    while (!(ehci_read(op, EHCI_USBSTS) & EHCI_STS_HALTED)) {
        if (usb_time_ms() - start > 20) break;
    }
    ehci_write(op, EHCI_USBSTS, 0x3F);      // Clear pending status

    // Every port to the companion controllers. Clearing CONFIGFLAG should
    // set every port owner bit by itself; set them explicitly as well for
    // controllers (and emulators) that do not.
    ehci_write(op, EHCI_CONFIGFLAG, 0);
    uint32_t ports = ehci_read(cap, EHCI_HCSPARAMS) & 0x0F;
    for (uint32_t i = 0; i < ports; i++) {
        uint32_t v = ehci_read(op, EHCI_PORTSC + 4 * i);
        if (!(v & EHCI_PORT_OWNER))
            ehci_write(op, EHCI_PORTSC + 4 * i, (v & ~EHCI_PORT_RWC) | EHCI_PORT_OWNER);
    }
    usb_delay_ms(10);
}
