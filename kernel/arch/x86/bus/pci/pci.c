#include <kernel/arch/x86/bus/pci/pci.h>
#include <kernel/arch/x86/bus/pci/pci_internal.h>
#include <kernel/arch/x86/bus/usb/usb.h>

#include <kernel/kernel.h>
#include <kernel/knode.h>

// Class drivers, first match wins. Functions nothing matches still get
// their /dev/pci entry.
static const struct PCIClassDriver pci_class_drivers[] = {
    { PCI_CLASS_STORAGE,    PCI_SUBCLASS_IDE,  PCI_ANY, pci_storage_probe_ide  },
    { PCI_CLASS_STORAGE,    PCI_SUBCLASS_SATA, PCI_ANY, pci_storage_probe_ahci },
    { PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB,  PCI_ANY, usb_pci_probe          },
};

#define PCI_CLASS_DRIVER_COUNT  (sizeof(pci_class_drivers) / sizeof(pci_class_drivers[0]))

void pci_probe_driver(const struct PCIDeviceInfo* info, struct PCIScanContext* ctx) {
    for (uint32_t i = 0; i < PCI_CLASS_DRIVER_COUNT; i++) {
        const struct PCIClassDriver* drv = &pci_class_drivers[i];
        if (drv->class_code != info->class_code) continue;
        if (drv->subclass != PCI_ANY && drv->subclass != info->subclass) continue;
        if (drv->prog_if != PCI_ANY && drv->prog_if != info->prog_if) continue;

        drv->probe(info, ctx);
        return;
    }
}

void pci_init(void) {
    void* root_node = knode_get_root();
    void* dev_directory = knode_find_by_name(root_node, "dev");
    void* mnt_directory = knode_find_by_name(root_node, "mnt");
    void* pci_directory = create_knode("pci", dev_directory);

    void* bus0_directory = create_knode("bus0", pci_directory);

    knode_set_permissions(pci_directory, KMALLOC_PERMISSION_READ);
    knode_set_permissions(bus0_directory, KMALLOC_PERMISSION_READ);

    // Read the paths once, let every storage device on every bus update the
    // same copy, then publish it once
    struct LocalPaths fs_paths;
    kernel_get_local_paths(&fs_paths);

    struct PCIScanContext ctx = { mnt_directory, &fs_paths };

    pci_scan_reset();
    pci_scan_bus(0, bus0_directory, &ctx, 0);

    kernel_set_local_paths(&fs_paths);

    // USB controllers are only recorded during the scan. They start once
    // the whole tree is known, so an EHCI controller can hand its ports to
    // its companions before those enumerate them.
    usb_init();
}
