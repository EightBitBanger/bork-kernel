#ifndef KERNEL_DRIVERS_AHCI_H
#define KERNEL_DRIVERS_AHCI_H

#include <stdint.h>
#include <stdbool.h>

#define AHCI_DEV_SATA          0x00000101
#define AHCI_DEV_SATAPI        0x00000102
#define AHCI_DEV_SEMB          0x00000103
#define AHCI_DEV_PM            0x00000104

#define ATA_CMD_READ_DMA_EX          0x25
#define ATA_CMD_WRITE_DMA_EX         0x35
#define ATA_CMD_FLUSH_CACHE_EX       0xEA

// Physical Region Descriptor Table (PRDT) entry manages data buffers
struct AHCI_PRDT_Entry {
    uint32_t data_base_addr;       // 32-bit physical address of data buffer
    uint32_t data_base_addr_upper; // Upper 32-bits for 64-bit architectures
    uint32_t reserved0;
    uint32_t byte_count : 22;      // Bit 0-21: Number of bytes to transfer (max 4MB)
    uint32_t reserved1  : 9;       // Bit 22-30
    uint32_t interrupt  : 1;       // Bit 31: Interrupt on completion
};

// One 4 KB command table holds the 128-byte header plus this many PRDT entries
#define AHCI_MAX_PRDT            248
#define AHCI_PRDT_MAX_BYTES      0x400000U   // 4 MB per PRDT entry

// Command table containing the structural components of a specific command
struct AHCI_Command_Table {
    uint8_t  command_fis[64];       // Frame Information Structure command packet
    uint8_t  atapi_command[16];     // ATAPI command packet setup space
    uint8_t  reserved[48];
    struct AHCI_PRDT_Entry prdt_entries[AHCI_MAX_PRDT]; // Scatter/gather list (one entry per physical run)
};

// Command Header structure located within the Command List array
struct AHCI_Command_Header {
    uint8_t  fis_length : 5;  // Length of Command FIS packet in DWORDS
    uint8_t  atapi      : 1;  // Proves ATAPI hardware control command
    uint8_t  write      : 1;  // Direction: 1 = RAM to device, 0 = Device to RAM
    uint8_t  prefetchable:1;  // Hardware optimization flag
    
    uint8_t  clear_busy : 1;  // Reset connection lines on busy
    uint8_t  reserved0  : 3;
    uint8_t  pmp        : 4;  // Port multiplier port assignment
    
    uint16_t prdt_length;     // Real total entry capacity inside the table list
    volatile uint32_t prd_bytes_transferred;
    uint32_t command_table_base_addr; // Physical pointer address targeting command structures
    uint32_t command_table_base_addr_upper;
    uint32_t reserved1[4];
};

// Individual storage controller channel data port architecture registers
struct AHCI_Port_Registers {
    volatile uint32_t command_list_base;         
    volatile uint32_t command_list_base_upper;
    volatile uint32_t fis_base;                  
    volatile uint32_t fis_base_upper;
    volatile uint32_t interrupt_status;
    volatile uint32_t interrupt_enable;
    volatile uint32_t command_and_status;
    volatile uint32_t reserved0;
    volatile uint32_t task_file_data;
    volatile uint32_t signature;
    volatile uint32_t sata_status;
    volatile uint32_t sata_control;
    volatile uint32_t sata_error;
    volatile uint32_t sata_active;
    volatile uint32_t command_issue;
    volatile uint32_t sata_notification;
    volatile uint32_t fis_switch_control;
    uint32_t reserved1[11];
    uint32_t vendor_specific[4];
};

struct AHCI_HBA_Memory_Space {
    volatile uint32_t host_capabilities;
    volatile uint32_t global_host_control;
    volatile uint32_t interrupt_status;
    volatile uint32_t ports_implemented;
    volatile uint32_t version;
    volatile uint32_t command_completion_coalescing_control;
    volatile uint32_t command_completion_coalescing_ports;
    volatile uint32_t enclosure_management_location;
    volatile uint32_t enclosure_management_control;
    volatile uint32_t host_capabilities_extended;
    volatile uint32_t bios_os_handoff_control;
    uint8_t  reserved[116];
    uint8_t  vendor_specific[96];
    struct AHCI_Port_Registers ports[32]; 
};

// Initiate the AHCI driver
void ahci_init(void* abar_virtual_address);

// Size of the HBA register block that must be mapped (0x1100 bytes for 32 ports)
#define AHCI_HBA_MMIO_SIZE  ((uint32_t)sizeof(struct AHCI_HBA_Memory_Space))

// Read from an AHCI port.
// The buffer may span pages; each physically contiguous run gets its own PRDT entry.
// It must be 2-byte aligned and mapped.
bool ahci_read_sectors(struct AHCI_Port_Registers* port, uint64_t start_lba, uint32_t count, uint8_t* buffer);

// Write to an AHCI port
bool ahci_write_sectors(struct AHCI_Port_Registers* port, uint64_t start_lba, uint32_t count, const uint8_t* buffer);

// Commit the drive's volatile write cache to the media (FLUSH CACHE EXT).
// Call before power-off; without it, recently written sectors can be lost.
bool ahci_flush_cache(struct AHCI_Port_Registers* port);

// Get an AHCI device port (of the most recently initialized controller)
struct AHCI_Port_Registers* ahci_get_port(int port_num);

// True if ahci_init() brought this port up (SATA disk attached, DMA
// structures allocated, command engine running). Only these can be used.
bool ahci_port_ready(struct AHCI_Port_Registers* port);

// Every port brought up so far, across all controllers, in setup order
uint32_t ahci_ready_port_count(void);
struct AHCI_Port_Registers* ahci_get_ready_port(uint32_t index);

// Disk size from IDENTIFY DEVICE (LBA48 when supported), 0 on failure
uint64_t ahci_identify_sector_count(struct AHCI_Port_Registers* port);

// Register a ready port as a generic block device ("ahci0", "ahci1", ...).
// Returns the existing device if the port is already registered.
struct BlockDevice;
struct BlockDevice* ahci_create_block_device(struct AHCI_Port_Registers* port);

#endif
