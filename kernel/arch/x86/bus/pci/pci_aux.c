#include <kernel/arch/x86/io.h>
#include <kernel/arch/x86/irq.h>
#include <kernel/arch/x86/heap.h>
#include <kernel/arch/x86/bus/pci/pci.h>
#include <kernel/arch/x86/bus/pci/pci_internal.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/arch/x86/drivers/ahci/ahci.h>
#include <kernel/arch/x86/drivers/ata/ata.h>

#include <kernel/kernel.h>
#include <kernel/knode.h>
#include <kernel/fs/fs.h>

#include <kernel/console/print.h>
#include <kernel/util/string.h>

#define PCI_COMMAND_IO          (1U << 0)
#define PCI_COMMAND_MEMORY      (1U << 1)

// The CF8/CFC address+data pair must not be split by another config access
// (e.g. a driver on another thread), so each access runs with IRQs off.

uint32_t pci_config_read(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)(((uint32_t)bus << 16) | ((uint32_t)device << 11) |
                      ((uint32_t)func << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    uint32_t flags = irq_save();
    outl(0xCF8, address);
    uint32_t value = inl(0xCFC);
    irq_restore(flags);
    return value;
}

void pci_config_write(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address = (uint32_t)(((uint32_t)bus << 16) | ((uint32_t)device << 11) |
                      ((uint32_t)func << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    uint32_t flags = irq_save();
    outl(0xCF8, address);
    outl(0xCFC, value);
    irq_restore(flags);
}

// Offset 0x04 holds Command (low 16 bits) and Status (high 16 bits). Status
// bits are write-1-to-clear, so writing back the full dword read from the
// register clears any pending status. Only ever write the command half.
uint16_t pci_read_command(uint8_t bus, uint8_t dev, uint8_t func) {
    return (uint16_t)(pci_config_read(bus, dev, func, 0x04) & 0xFFFF);
}

void pci_write_command(uint8_t bus, uint8_t dev, uint8_t func, uint16_t command) {
    pci_config_write(bus, dev, func, 0x04, (uint32_t)command);
}

void pci_enable_command_bits(uint8_t bus, uint8_t dev, uint8_t func, uint16_t bits) {
    pci_write_command(bus, dev, func, (uint16_t)(pci_read_command(bus, dev, func) | bits));
}

static uint8_t pci_bar_count(uint8_t bus, uint8_t dev, uint8_t func) {
    uint8_t header_type = (uint8_t)((pci_config_read(bus, dev, func, 0x0C) >> 16) & 0x7F);
    switch (header_type) {
        case 0x00: return 6;   // General device
        case 0x01: return 2;   // PCI-to-PCI bridge
        case 0x02: return 1;   // CardBus bridge
        default:   return 0;
    }
}

uint8_t pci_read_bar(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_index, struct PCIBar* out) {
    out->type = PCI_BAR_NONE;
    out->prefetchable = false;
    out->address = 0;
    out->size = 0;

    uint8_t bar_count = pci_bar_count(bus, dev, func);
    if (bar_index >= bar_count)
        return 0;

    uint8_t  offset = (uint8_t)(0x10 + bar_index * 4);
    uint32_t low    = pci_config_read(bus, dev, func, offset);

    bool is_io  = (low & 0x01) != 0;
    bool is_64  = !is_io && ((low >> 1) & 0x03) == 0x02;
    uint8_t slots = 1;

    if (is_64 && bar_index + 1 >= bar_count)
        return 1;   // Malformed: 64-bit BAR in the last slot
    if (is_64)
        slots = 2;

    // Switch decoding off while the BAR holds all-ones, otherwise the device
    // can briefly claim a random address range (often over RAM or the APIC).
    uint32_t flags = irq_save();
    uint16_t command = pci_read_command(bus, dev, func);
    pci_write_command(bus, dev, func, (uint16_t)(command & ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY)));

    pci_config_write(bus, dev, func, offset, 0xFFFFFFFF);
    uint32_t low_mask = pci_config_read(bus, dev, func, offset);
    pci_config_write(bus, dev, func, offset, low);

    uint32_t high = 0, high_mask = 0;
    if (is_64) {
        uint8_t high_offset = (uint8_t)(offset + 4);
        high = pci_config_read(bus, dev, func, high_offset);
        pci_config_write(bus, dev, func, high_offset, 0xFFFFFFFF);
        high_mask = pci_config_read(bus, dev, func, high_offset);
        pci_config_write(bus, dev, func, high_offset, high);
    }

    pci_write_command(bus, dev, func, command);
    irq_restore(flags);

    if (is_io) {
        uint32_t mask = low_mask & 0xFFFFFFFCU;
        if (mask == 0) return slots;
        out->type    = PCI_BAR_IO;
        out->address = low & 0xFFFFFFFCU;
        out->size    = (uint64_t)((~mask + 1U) & 0xFFFFU);
        return slots;
    }

    // An unimplemented BAR reads back zero after the all-ones write
    if ((low_mask & 0xFFFFFFF0U) == 0 && high_mask == 0)
        return slots;

    uint64_t mask = ((uint64_t)high_mask << 32) | (low_mask & 0xFFFFFFF0U);
    if (!is_64)
        mask |= 0xFFFFFFFF00000000ULL;   // Upper half is implicitly all-ones

    uint64_t size = ~mask + 1ULL;
    if (size == 0)
        return slots;

    out->type         = is_64 ? PCI_BAR_MEM64 : PCI_BAR_MEM32;
    out->prefetchable = (low & 0x08) != 0;
    out->address      = ((uint64_t)high << 32) | (low & 0xFFFFFFF0U);
    out->size         = size;
    return slots;
}

void* pci_map_bar(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_index, uint32_t max_bytes, uint32_t flags) {
    struct PCIBar bar;
    if (pci_read_bar(bus, dev, func, bar_index, &bar) == 0)
        return NULL;

    if (bar.type != PCI_BAR_MEM32 && bar.type != PCI_BAR_MEM64)
        return NULL;
    if (bar.address == 0)
        return NULL;   // Not assigned by firmware

    uint64_t map_size = bar.size;
    if (max_bytes != 0 && map_size > max_bytes)
        map_size = max_bytes;

    // No PAE: the mapped range must sit entirely below 4 GB
    if (bar.address + map_size - 1 > 0xFFFFFFFFULL)
        return NULL;

    return vmm_map_mmio_region((uint32_t)bar.address, (uint32_t)map_size, flags);
}

void* pci_map_device_bars(uint8_t bus, uint8_t dev, uint8_t func) {
    void* first_mapped_vaddr = NULL;
    uint32_t mmio_flags = VM_PRESENT | VM_READWRITE | VM_PCD;

    uint8_t bar_index = 0;
    while (1) {
        struct PCIBar bar;
        uint8_t slots = pci_read_bar(bus, dev, func, bar_index, &bar);
        if (slots == 0) break;

        if (bar.type == PCI_BAR_MEM32 || bar.type == PCI_BAR_MEM64) {
            void* virt_addr = pci_map_bar(bus, dev, func, bar_index, 0, mmio_flags);
            if (virt_addr != NULL && first_mapped_vaddr == NULL) {
                first_mapped_vaddr = virt_addr;
            }
        }
        bar_index = (uint8_t)(bar_index + slots);
    }
    return first_mapped_vaddr;
}

uint16_t pci_get_io_bar(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_index) {
    uint8_t bar_offset = 0x10 + (bar_index * 4);
    uint32_t bar = pci_config_read(bus, dev, func, bar_offset);

    if (bar != 0 && (bar & 0x01) == 1) {
        return (uint16_t)(bar & 0xFFFFFFFC);
    }
    return 0;
}
