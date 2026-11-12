#ifndef KERNEL_DRIVERS_NVME_H
#define KERNEL_DRIVERS_NVME_H

#include <stdint.h>
#include <stdbool.h>

// NVMe (NVM Express) storage
//
// Polled, one I/O queue pair per controller, one command in flight at a
// time. Every namespace of a controller (formatted with 512..4096-byte
// blocks and no metadata) becomes a block device: "nvme0", "nvme1", ...
//
// The controller is not trusted: register values (doorbell stride, queue
// sizes, timeouts) and everything IDENTIFY returns are range-checked before
// use, completions are matched against the command that was issued, and a
// command that never completes disables the controller, so it cannot DMA
// into a buffer the kernel has since reused.

// Bytes of BAR0 to map: the controller registers (0x1000) and the admin and
// I/O queue doorbells, for doorbell strides up to 512 bytes
#define NVME_MMIO_SIZE  0x2000U

struct NvmeController;
struct NvmeNamespace;

// Bring up the controller whose BAR0 is mapped at `regs` (`regs_size` bytes
// of it). `pci_location` (bus << 8 | dev << 3 | func) tells controllers
// apart, so a second PCI scan finds the one already running. NULL if the
// controller is unusable.
struct NvmeController* nvme_init(volatile void* regs, uint32_t regs_size, uint16_t pci_location);

// A controller nvme_init() already brought up, or NULL
struct NvmeController* nvme_find_controller(uint16_t pci_location);

// Namespaces of a controller that can be used as block devices
uint32_t              nvme_namespace_count(const struct NvmeController* ctrl);
struct NvmeNamespace* nvme_get_namespace(struct NvmeController* ctrl, uint32_t index);

// Read / write whole blocks of a namespace (its own block size). The buffer
// must be mapped kernel memory; it may span pages.
bool nvme_read_blocks(struct NvmeNamespace* ns, uint64_t lba, uint32_t count, void* buffer);
bool nvme_write_blocks(struct NvmeNamespace* ns, uint64_t lba, uint32_t count, const void* buffer);

// Commit the volatile write cache, if the controller has one
bool nvme_flush(struct NvmeNamespace* ns);

// Register a namespace as a generic block device ("nvme0", "nvme1", ...).
// Returns the existing device if the namespace is already registered.
struct BlockDevice;
struct BlockDevice* nvme_create_block_device(struct NvmeNamespace* ns);

#endif
