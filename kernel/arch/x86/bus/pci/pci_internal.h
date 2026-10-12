#ifndef _PCI_BUS_INTERNAL_H_
#define _PCI_BUS_INTERNAL_H_

#include <kernel/kernel.h>
#include <kernel/knode.h>
#include <kernel/fs/fs.h>

#include <stdint.h>

#include <kernel/arch/x86/bus/pci/pci.h>

#define PCI_MAX_BRIDGE_DEPTH    16

// Hardware detection
//
// fs_paths is owned by pci_init(): it is read once before the scan, updated
// in place by every storage device found (including ones behind bridges),
// and written back once after the whole tree is scanned.

void pci_scan_reset(void);
void pci_scan_bus(uint8_t bus_number, uint32_t pci_directory, uint32_t mnt_directory, struct LocalPaths* fs_paths, uint8_t depth);

// handle device classes

void pci_handle_storage_device(uint8_t bus, uint8_t dev, uint8_t func, uint8_t subclass, uint8_t prog_if, uint32_t mnt_dir, struct LocalPaths* fs_paths);

// IO

uint32_t pci_config_read(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset);
void pci_config_write(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t value);

uint16_t pci_read_command(uint8_t bus, uint8_t dev, uint8_t func);
void pci_write_command(uint8_t bus, uint8_t dev, uint8_t func, uint16_t command);
void pci_enable_command_bits(uint8_t bus, uint8_t dev, uint8_t func, uint16_t bits);

uint16_t pci_get_io_bar(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_index);

// Properties

uint32_t pci_create_property_node(uint32_t parent_knode, const char* name, const char* value);
void pci_export_vfs_properties(uint32_t type_knode, const struct PCIDeviceInfo* dev_info);

// String formatting

const char* pci_get_class_name(uint8_t class_code);
void format_device_string(char* buf, uint8_t dev, uint8_t func);
void format_bus_string(char* buf, uint8_t bus);

void format_hex32_string(char* buf, uint32_t val);
void format_hex16_string(char* buf, uint16_t val);
void format_hex8_string(char* buf, uint8_t val);
void format_dec8_string(char* buf, uint8_t val);

#endif
