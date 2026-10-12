#include <kernel/arch/x86/io.h>
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

// One bit per bus number, so a misconfigured bridge (secondary bus pointing
// back at an ancestor, or two bridges claiming the same bus) cannot make the
// recursive scan loop or enumerate a bus twice. Reset by pci_scan_reset().
static uint32_t pci_buses_scanned[256 / 32];

void pci_scan_reset(void) {
    memset(pci_buses_scanned, 0, sizeof(pci_buses_scanned));
}

static bool pci_bus_claim(uint8_t bus) {
    uint32_t word = bus / 32U, bit = 1U << (bus % 32U);
    if (pci_buses_scanned[word] & bit) return false;
    pci_buses_scanned[word] |= bit;
    return true;
}

// Physical address of the first memory BAR, for the "bar" property node.
// Nothing is mapped here: mapping every BAR of every device during the scan
// exhausted the 256 MB kernel MMIO window on hardware with large BARs (GPUs).
static uint64_t pci_first_memory_bar(uint8_t bus, uint8_t dev, uint8_t func) {
    uint8_t bar_index = 0;
    while (1) {
        struct PCIBar bar;
        uint8_t slots = pci_read_bar(bus, dev, func, bar_index, &bar);
        if (slots == 0) break;

        if ((bar.type == PCI_BAR_MEM32 || bar.type == PCI_BAR_MEM64) && bar.address != 0)
            return bar.address;

        bar_index = (uint8_t)(bar_index + slots);
    }
    return 0;
}

static void pci_scan_function(uint8_t bus, uint8_t dev, uint8_t func, uint32_t pci_dir, uint32_t mnt_dir, struct LocalPaths* fs_paths, uint8_t depth) {
    uint32_t id_reg = pci_config_read(bus, dev, func, 0);
    uint16_t v_id = (uint16_t)(id_reg & 0xFFFF);
    uint16_t d_id = (uint16_t)((id_reg >> 16) & 0xFFFF);

    if (v_id == 0xFFFF || v_id == 0x0000)
        return;

    uint32_t class_reg = pci_config_read(bus, dev, func, 0x08);
    uint8_t class_code = (uint8_t)((class_reg >> 24) & 0xFF);
    uint8_t subclass   = (uint8_t)((class_reg >> 16) & 0xFF);
    uint8_t prog_if    = (uint8_t)((class_reg >> 8) & 0xFF);

    uint32_t sub_reg   = pci_config_read(bus, dev, func, 0x2C);
    uint16_t sub_v_id  = (uint16_t)(sub_reg & 0xFFFF);
    uint16_t sub_id    = (uint16_t)((sub_reg >> 16) & 0xFFFF);

    uint32_t intr_reg  = pci_config_read(bus, dev, func, 0x3C);
    uint8_t irq_line   = (uint8_t)(intr_reg & 0xFF);
    uint8_t irq_pin    = (uint8_t)((intr_reg >> 8) & 0xFF);

    char device_node_name[16];
    format_device_string(device_node_name, dev, func);

    uint32_t device_knode = create_knode(device_node_name, pci_dir);
    const char* type_name = pci_get_class_name(class_code);
    uint32_t type_knode = create_knode(type_name, device_knode);

    knode_set_permissions(device_knode, KMALLOC_PERMISSION_READ);
    knode_set_permissions(type_knode, KMALLOC_PERMISSION_READ);

    if (class_code == 0x01) {
        pci_handle_storage_device(bus, dev, func, subclass, prog_if, mnt_dir, fs_paths);
    }

    struct PCIDeviceInfo dev_info = {
        .vendor_id = v_id,
        .device_id = d_id,
        .class_code = class_code,
        .subclass = subclass,
        .sub_vendor_id = sub_v_id,
        .sub_device_id = sub_id,
        .irq_line = irq_line,
        .irq_pin = irq_pin,
        .bar_physical_address = pci_first_memory_bar(bus, dev, func)
    };
    pci_export_vfs_properties(type_knode, &dev_info);

    // Handle PCI-to-PCI Bridges
    if (class_code == 0x06 && subclass == 0x04) {
        uint32_t bus_reg = pci_config_read(bus, dev, func, 0x18);
        uint8_t secondary_bus = (uint8_t)((bus_reg >> 8) & 0xFF);

        // Bus 0 is never a secondary bus; anything else already seen is a loop
        if (secondary_bus == 0 || secondary_bus == bus || depth + 1 > PCI_MAX_BRIDGE_DEPTH)
            return;

        char sub_bus_name[16];
        format_bus_string(sub_bus_name, secondary_bus);

        uint32_t secondary_bus_directory = create_knode(sub_bus_name, type_knode);
        knode_set_permissions(secondary_bus_directory, KMALLOC_PERMISSION_READ);

        // Same fs_paths object all the way down, so storage found behind the
        // bridge is not overwritten when the parent scan finishes.
        pci_scan_bus(secondary_bus, secondary_bus_directory, mnt_dir, fs_paths, (uint8_t)(depth + 1));
    }
}

void pci_scan_bus(uint8_t bus_number, uint32_t pci_directory, uint32_t mnt_directory, struct LocalPaths* fs_paths, uint8_t depth) {
    if (!pci_bus_claim(bus_number))
        return;

    for (uint8_t dev = 0; dev < 32; dev++) {
        uint32_t reg0 = pci_config_read(bus_number, dev, 0, 0);
        uint16_t vendor_id = (uint16_t)(reg0 & 0xFFFF);

        if (vendor_id == 0xFFFF || vendor_id == 0x0000)
            continue;

        uint32_t header_reg = pci_config_read(bus_number, dev, 0, 0x0C);
        uint8_t header_type = (uint8_t)((header_reg >> 16) & 0xFF);
        uint8_t max_functions = (header_type & 0x80) ? 8 : 1;

        for (uint8_t func = 0; func < max_functions; func++) {
            pci_scan_function(bus_number, dev, func, pci_directory, mnt_directory, fs_paths, depth);
        }
    }
}
