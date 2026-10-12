#ifndef _PCI_BUS_H_
#define _PCI_BUS_H_

#include <stdint.h>
#include <stdbool.h>

// BAR decode result
#define PCI_BAR_NONE    0   // Unimplemented / out of range
#define PCI_BAR_IO      1   // I/O port space
#define PCI_BAR_MEM32   2   // 32-bit memory space
#define PCI_BAR_MEM64   3   // 64-bit memory space (consumes two BAR slots)

struct PCIBar {
    uint8_t  type;          // PCI_BAR_*
    bool     prefetchable;  // Memory BARs only
    uint64_t address;       // Physical address (I/O port for PCI_BAR_IO)
    uint64_t size;          // Decoded size in bytes
};

struct PCIDeviceInfo {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint16_t sub_vendor_id;
    uint16_t sub_device_id;
    uint8_t irq_line;
    uint8_t irq_pin;

    // Physical address of the first memory BAR (0 if none). BARs are no
    // longer mapped during the bus scan; drivers map what they need with
    // pci_map_bar().
    uint64_t bar_physical_address;
};

void pci_init(void);

// Mount what a disk holds at run time (its basefs partitions, or the disk
// itself), using the same rules as the boot-time scan
struct BlockDevice;
void pci_mount_storage(struct BlockDevice* disk);

// Probe one BAR. Memory/I/O decoding is switched off while the BAR is sized
// so the temporary all-ones value never decodes on the bus.
// Returns how many BAR slots it occupies (2 for a 64-bit BAR, 1 otherwise),
// or 0 if bar_index is out of range for this header type.
uint8_t pci_read_bar(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_index, struct PCIBar* out);

// Map a memory BAR into kernel virtual space. max_bytes limits the mapping
// (0 = whole BAR); use it for devices whose BAR is far larger than the
// registers the driver touches. Returns NULL for I/O BARs, BARs above 4 GB,
// or when the virtual window is exhausted.
void* pci_map_bar(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_index, uint32_t max_bytes, uint32_t flags);

// Legacy helper: maps every memory BAR of a function and returns the first.
// Avoid on devices with large BARs (GPUs); prefer pci_map_bar().
void* pci_map_device_bars(uint8_t bus, uint8_t dev, uint8_t func);

#endif
