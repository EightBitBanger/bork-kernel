#ifndef _PCI_BUS_H_
#define _PCI_BUS_H_

#include <stdint.h>
#include <stdbool.h>

//
// PCI bus
//
//   pci.c          pci_init(), the class driver table
//   pci_scan.c     bus walk, bridges, the /dev/pci tree
//   pci_config.c   configuration space, command register, BARs
//   pci_storage.c  IDE / AHCI controllers and mounting their disks
//   pci_prop.c     /dev/pci property nodes
//   pci_strings.c  name formatting
//
// Drivers only need this header: config space access, BAR mapping and the
// class codes below.
//

// Command register bits (config offset 0x04)
#define PCI_COMMAND_IO              (1U << 0)
#define PCI_COMMAND_MEMORY          (1U << 1)
#define PCI_COMMAND_BUS_MASTER      (1U << 2)
#define PCI_COMMAND_INTX_DISABLE    (1U << 10)

// Class codes
#define PCI_CLASS_LEGACY            0x00
#define PCI_CLASS_STORAGE           0x01
#define PCI_CLASS_NETWORK           0x02
#define PCI_CLASS_DISPLAY           0x03
#define PCI_CLASS_MULTIMEDIA        0x04
#define PCI_CLASS_BRIDGE            0x06
#define PCI_CLASS_SERIAL_BUS        0x0C

#define PCI_SUBCLASS_IDE            0x01    // PCI_CLASS_STORAGE
#define PCI_SUBCLASS_SATA           0x06    // PCI_CLASS_STORAGE
#define PCI_SUBCLASS_PCI_BRIDGE     0x04    // PCI_CLASS_BRIDGE
#define PCI_SUBCLASS_USB            0x03    // PCI_CLASS_SERIAL_BUS

// USB host controller interfaces (prog_if of PCI_SUBCLASS_USB)
#define PCI_USB_UHCI                0x00
#define PCI_USB_OHCI                0x10
#define PCI_USB_EHCI                0x20
#define PCI_USB_XHCI                0x30

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

// One function found by the bus scan, as handed to class drivers
struct PCIDeviceInfo {
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;

    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint16_t sub_vendor_id;
    uint16_t sub_device_id;
    uint8_t  irq_line;
    uint8_t  irq_pin;

    // Physical address of the first memory BAR (0 if none). BARs are not
    // mapped during the bus scan; drivers map what they need with
    // pci_map_bar().
    uint64_t bar_physical_address;
};

void pci_init(void);

// Mount what a disk holds at run time (its basefs partitions, or the disk
// itself), using the same rules as the boot-time scan
struct BlockDevice;
void pci_mount_storage(struct BlockDevice* disk);
// Same, printing nothing: for a disk rewritten while the desktop owns the
// screen (SYS_FORMAT), where kernel console text would land on top of it
void pci_mount_storage_quiet(struct BlockDevice* disk);
// Same for a removable (USB) disk, printing nothing when `quiet` (plugged in
// at run time). A disk whose partitions are all of unknown filesystems (a FAT
// stick) is mounted whole as unformatted, so it shows up and can be formatted.
void pci_mount_removable(struct BlockDevice* disk, bool quiet);

// What the loader booted from, as far as the disks pci_init() mounted can
// tell. The boot disk is found by its disk signature (see pci_storage.c).
enum PciBootMedia {
    PCI_BOOT_UNKNOWN,       // No boot area to compare with (booted by another loader)
    PCI_BOOT_SYSTEM_DISK,   // A fixed (ATA / AHCI) disk, and home's /sys is on it
    PCI_BOOT_MEDIA,         // A USB disk, a disk without the system on it (a stick
                            // from build-image.sh, an install disk), or one the
                            // kernel cannot reach
};

enum PciBootMedia pci_boot_media(void);

//
// Configuration space
//
// Offsets are in bytes. The 8/16-bit forms access only their own bytes, so
// writing one register never rewrites (and clears) RW1C bits next to it.
//

uint32_t pci_config_read(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void     pci_config_write(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value);

uint16_t pci_config_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void     pci_config_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint16_t value);

uint8_t  pci_config_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void     pci_config_write8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint8_t value);

uint16_t pci_read_command(uint8_t bus, uint8_t dev, uint8_t func);
void     pci_write_command(uint8_t bus, uint8_t dev, uint8_t func, uint16_t command);
void     pci_enable_command_bits(uint8_t bus, uint8_t dev, uint8_t func, uint16_t bits);

//
// BARs
//

// Probe one BAR. Memory/I/O decoding is switched off while the BAR is sized
// so the temporary all-ones value never decodes on the bus.
// Returns how many BAR slots it occupies (2 for a 64-bit BAR, 1 otherwise),
// or 0 if bar_index is out of range for this header type.
uint8_t pci_read_bar(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_index, struct PCIBar* out);

// I/O port base of an I/O BAR, 0 if the BAR is not an assigned I/O BAR
uint16_t pci_get_io_bar(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_index);

// Map a memory BAR into kernel virtual space. max_bytes limits the mapping
// (0 = whole BAR); use it for devices whose BAR is far larger than the
// registers the driver touches. Returns NULL for I/O BARs, BARs above 4 GB,
// or when the virtual window is exhausted.
void* pci_map_bar(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_index, uint32_t max_bytes, uint32_t flags);

// Legacy helper: maps every memory BAR of a function and returns the first.
// Avoid on devices with large BARs (GPUs); prefer pci_map_bar().
void* pci_map_device_bars(uint8_t bus, uint8_t dev, uint8_t func);

#endif
