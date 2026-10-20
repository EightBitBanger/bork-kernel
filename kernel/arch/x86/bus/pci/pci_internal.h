#ifndef _PCI_BUS_INTERNAL_H_
#define _PCI_BUS_INTERNAL_H_

#include <kernel/kernel.h>
#include <kernel/knode.h>
#include <kernel/fs/fs.h>

#include <stdint.h>

#include <kernel/arch/x86/bus/pci/pci.h>

#define PCI_MAX_BRIDGE_DEPTH    16

// Matches any subclass / prog_if in a class driver entry
#define PCI_ANY                 0xFFFF

// State shared by everything one pci_init() run finds.
//
// fs_paths is read once before the scan, updated in place by every storage
// device found (including ones behind bridges), and written back once after
// the whole tree is scanned.
struct PCIScanContext {
    void* mnt_dir;
    struct LocalPaths* fs_paths;
};

// A driver for one kind of function. probe() runs during the scan, before
// the device's property nodes are written.
struct PCIClassDriver {
    uint8_t  class_code;
    uint16_t subclass;      // PCI_ANY or an exact value
    uint16_t prog_if;       // PCI_ANY or an exact value
    void (*probe)(const struct PCIDeviceInfo* info, struct PCIScanContext* ctx);
};

// Bus scan (pci_scan.c)

void pci_scan_reset(void);
void pci_scan_bus(uint8_t bus_number, void* pci_directory, struct PCIScanContext* ctx, uint8_t depth);

// Run the class driver for a function, if one matches (pci.c)
void pci_probe_driver(const struct PCIDeviceInfo* info, struct PCIScanContext* ctx);

// Storage controllers (pci_storage.c)

void pci_storage_probe_ide(const struct PCIDeviceInfo* info, struct PCIScanContext* ctx);
void pci_storage_probe_ahci(const struct PCIDeviceInfo* info, struct PCIScanContext* ctx);

// Properties (pci_prop.c)

void* pci_create_property_node(void* parent_knode, const char* name, const char* value);
void pci_export_vfs_properties(void* type_knode, const struct PCIDeviceInfo* dev_info);

// String formatting (pci_strings.c)

const char* pci_get_class_name(uint8_t class_code);
void format_device_string(char* buf, uint8_t dev, uint8_t func);
void format_bus_string(char* buf, uint8_t bus);

void format_hex32_string(char* buf, uint32_t val);
void format_hex16_string(char* buf, uint16_t val);
void format_hex8_string(char* buf, uint8_t val);
void format_dec8_string(char* buf, uint8_t val);

#endif
