#include <kernel/arch/x86/drivers/ahci/ahci.h>
#include <kernel/arch/x86/virtual/pmm.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/console/print.h>
#include <kernel/util/string.h>
#include <kernel/mutex.h>
#include <kernel/block/block.h>

#define AHCI_MAX_PORT_STATES  32
#define AHCI_SECTOR_SIZE      512

#define AHCI_PxCMD_ST         (1U << 0)
#define AHCI_PxCMD_FRE        (1U << 4)
#define AHCI_PxCMD_FR         (1U << 14)
#define AHCI_PxCMD_CR         (1U << 15)

#define AHCI_TFD_ERR          0x01
#define AHCI_TFD_DRQ          0x08
#define AHCI_TFD_BSY          0x80
#define AHCI_PxIS_TFES        (1U << 30)

// Per-port DMA structures. Previously these were single globals that were
// overwritten by every port (and every controller) that got initialized.
struct AhciPortState {
    struct AHCI_Port_Registers* port;
    uint8_t*  cmd_list;          // Virtual address of the 1 KB command list
    uint8_t*  fis_recv;          // Virtual address of the received-FIS area
    uint8_t*  cmd_table;         // Virtual address of the shared command table
    uint32_t  cmd_table_phys;    // Physical address handed to the HBA
    mutex_t   lock;              // One command at a time per port
};

static struct AhciPortState port_states[AHCI_MAX_PORT_STATES];
static uint32_t port_state_count = 0;

static struct AHCI_HBA_Memory_Space* global_hba = NULL;

// Allocate one zeroed, page-aligned page and report its physical address.
// vmm_alloc_pages() already backs the page with a PMM frame, so there is no
// need to allocate a second frame and remap (which leaked the first one).
static uint8_t* ahci_alloc_dma_page(uint32_t* phys_out) {
    uint8_t* virt = (uint8_t*)vmm_alloc_pages(1);
    if (!virt) return NULL;

    memset(virt, 0, PAGE_SIZE);
    *phys_out = vmm_get_phys_addr(virt);
    return virt;
}

static struct AhciPortState* ahci_find_state(struct AHCI_Port_Registers* port) {
    for (uint32_t i = 0; i < port_state_count; i++) {
        if (port_states[i].port == port) return &port_states[i];
    }
    return NULL;
}

bool ahci_port_ready(struct AHCI_Port_Registers* port) {
    return port != NULL && ahci_find_state(port) != NULL;
}

uint32_t ahci_ready_port_count(void) {
    return port_state_count;
}

struct AHCI_Port_Registers* ahci_get_ready_port(uint32_t index) {
    if (index >= port_state_count) return NULL;
    return port_states[index].port;
}

static void ahci_start_port(struct AHCI_Port_Registers* port) {
    // Wait until the controller command engine is completely idle
    uint32_t timeout = 1000000;
    while ((port->command_and_status & AHCI_PxCMD_CR) && timeout > 0) {
        timeout--;
    }

    // Enable FIS Receive FIRST so the HBA can communicate with the drive
    port->command_and_status |= AHCI_PxCMD_FRE;

    // NOW it is safe to wait for the drive to drop BSY and DRQ
    timeout = 1000000;
    while ((port->task_file_data & (AHCI_TFD_BSY | AHCI_TFD_DRQ)) && timeout > 0) {
        timeout--;
    }

    // Finally, start processing commands
    port->command_and_status |= AHCI_PxCMD_ST;
}

static void ahci_stop_port(struct AHCI_Port_Registers* port) {
    port->command_and_status &= ~AHCI_PxCMD_ST;
    port->command_and_status &= ~AHCI_PxCMD_FRE;

    // Wait for CR and FR to clear with a timeout loop
    uint32_t timeout = 1000000;
    while (timeout > 0) {
        if (!(port->command_and_status & AHCI_PxCMD_CR) &&
            !(port->command_and_status & AHCI_PxCMD_FR)) {
            break;
        }
        timeout--;
    }
}

static bool ahci_setup_port(struct AHCI_Port_Registers* port) {
    if (port_state_count >= AHCI_MAX_PORT_STATES) {
        print("AHCI Error: too many ports.\n");
        return false;
    }

    struct AhciPortState* st = &port_states[port_state_count];
    memset(st, 0, sizeof(struct AhciPortState));

    uint32_t cmd_list_phys = 0, fis_recv_phys = 0, cmd_table_phys = 0;

    st->cmd_list  = ahci_alloc_dma_page(&cmd_list_phys);
    st->fis_recv  = ahci_alloc_dma_page(&fis_recv_phys);
    st->cmd_table = ahci_alloc_dma_page(&cmd_table_phys);

    if (!st->cmd_list || !st->fis_recv || !st->cmd_table ||
        !cmd_list_phys || !fis_recv_phys || !cmd_table_phys) {
        if (st->cmd_list)  vmm_free_pages(st->cmd_list, 1);
        if (st->fis_recv)  vmm_free_pages(st->fis_recv, 1);
        if (st->cmd_table) vmm_free_pages(st->cmd_table, 1);
        print("AHCI Error: failed to allocate port DMA structures.\n");
        return false;
    }

    st->port = port;
    st->cmd_table_phys = cmd_table_phys;
    mutex_init(&st->lock);

    ahci_stop_port(port);

    port->command_list_base       = cmd_list_phys;
    port->command_list_base_upper = 0;
    port->fis_base                = fis_recv_phys;
    port->fis_base_upper          = 0;

    // Clear any stale errors / interrupt status (write-1-to-clear)
    port->sata_error       = 0xFFFFFFFF;
    port->interrupt_status = 0xFFFFFFFF;

    ahci_start_port(port);

    port_state_count++;
    return true;
}

void ahci_init(void* abar_virtual_address) {
    struct AHCI_HBA_Memory_Space* hba = (struct AHCI_HBA_Memory_Space*)abar_virtual_address;
    global_hba = hba;

    // Force AHCI Mode Enable
    hba->global_host_control |= (1U << 31);

    uint32_t active_ports = hba->ports_implemented;
    for (int i = 0; i < 32; i++) {
        if (!(active_ports & (1U << i))) continue;

        struct AHCI_Port_Registers* port = &hba->ports[i];

        uint32_t status = port->sata_status;
        uint8_t ipm = (status >> 8) & 0x0F;
        uint8_t det = status & 0x0F;

        if (det == 0x03 && ipm == 0x01 && port->signature == AHCI_DEV_SATA &&
            ahci_find_state(port) == NULL) {
            ahci_setup_port(port);
        }
    }
}

// Get a port by its index
struct AHCI_Port_Registers* ahci_get_port(int port_num) {
    if (global_hba == NULL) return NULL;
    if (port_num < 0 || port_num >= 32) return NULL;
    if (!(global_hba->ports_implemented & (1U << port_num))) return NULL;

    return &global_hba->ports[port_num];
}

// Bring a port back after a task-file error or a timeout. On TFES the HBA
// halts the command engine with PxCI still set; without this every later
// command found another "free" slot, reused the single shared command table
// while the old slot was still pending, and timed out (5M spins each).
#define AHCI_PxCMD_CLO        (1U << 3)

static void ahci_recover_port(struct AHCI_Port_Registers* port) {
    // Stop the command engine; this also clears PxCI
    port->command_and_status &= ~AHCI_PxCMD_ST;
    uint32_t timeout = 1000000;
    while ((port->command_and_status & AHCI_PxCMD_CR) && timeout > 0) timeout--;

    // Clear latched errors and interrupt status (write-1-to-clear)
    port->sata_error       = 0xFFFFFFFF;
    port->interrupt_status = 0xFFFFFFFF;

    // Device still BSY/DRQ: command list override clears the task file bits
    if (port->task_file_data & (AHCI_TFD_BSY | AHCI_TFD_DRQ)) {
        port->command_and_status |= AHCI_PxCMD_CLO;
        timeout = 1000000;
        while ((port->command_and_status & AHCI_PxCMD_CLO) && timeout > 0) timeout--;
    }

    port->command_and_status |= AHCI_PxCMD_ST;
}

// Describe a virtual buffer to the HBA. Virtually contiguous memory from
// vmm_alloc_pages() is NOT physically contiguous, so each page is translated
// separately; adjacent physical runs are merged into one entry.
// Returns the number of PRDT entries used, or -1 if the buffer can't be described.
static int ahci_build_prdt(struct AHCI_Command_Table* table, const uint8_t* buffer, uint32_t bytes) {
    if (bytes == 0 || ((uint32_t)buffer & 1U)) return -1;   // PRDT DBA must be word aligned

    int entries = 0;
    uint32_t vaddr = (uint32_t)buffer;
    uint32_t remaining = bytes;

    while (remaining > 0) {
        uint32_t page_left = PAGE_SIZE - (vaddr & (PAGE_SIZE - 1));
        uint32_t chunk = (remaining < page_left) ? remaining : page_left;

        uint32_t phys = vmm_get_phys_addr((void*)vaddr);
        if (phys == 0) return -1;   // Unmapped

        bool merged = false;
        if (entries > 0) {
            struct AHCI_PRDT_Entry* prev = &table->prdt_entries[entries - 1];
            uint32_t prev_len = (uint32_t)prev->byte_count + 1;
            if (prev->data_base_addr + prev_len == phys &&
                prev_len + chunk <= AHCI_PRDT_MAX_BYTES) {
                prev->byte_count = prev_len + chunk - 1;
                merged = true;
            }
        }

        if (!merged) {
            if (entries >= AHCI_MAX_PRDT) return -1;
            struct AHCI_PRDT_Entry* e = &table->prdt_entries[entries];
            e->data_base_addr       = phys;
            e->data_base_addr_upper = 0;
            e->byte_count           = chunk - 1;   // Encoded as length - 1
            e->interrupt            = 0;
            entries++;
        }

        vaddr     += chunk;
        remaining -= chunk;
    }

    return entries;
}

// Issue one command on a port and poll until it completes.
// buffer == NULL means a non-data command (no PRDT), e.g. FLUSH CACHE EXT.
static bool ahci_execute(struct AHCI_Port_Registers* port, uint8_t command, uint64_t start_lba,
                         uint32_t count, uint8_t* buffer, bool write, uint32_t complete_spins) {
    struct AhciPortState* st = ahci_find_state(port);
    if (!st) {
        print("AHCI Error: port not initialized.\n");
        return false;
    }

    mutex_lock(&st->lock);
    bool ok = false;

    // One shared command table per port, one command at a time: always
    // slot 0. If it is still busy, a previous command was left pending.
    int slot = 0;
    if ((port->command_issue | port->sata_active) & 1U) {
        ahci_recover_port(port);
        if ((port->command_issue | port->sata_active) & 1U) goto done;
    }

    // Wait for the device to be idle before issuing
    uint32_t timeout = 1000000;
    while ((port->task_file_data & (AHCI_TFD_BSY | AHCI_TFD_DRQ)) && timeout > 0) {
        timeout--;
    }
    if (timeout == 0) {
        print("AHCI Error: device busy.\n");
        goto fail;
    }

    // Clear pending interrupt status and errors (write-1-to-clear)
    port->interrupt_status = 0xFFFFFFFF;
    port->sata_error       = 0xFFFFFFFF;

    struct AHCI_Command_Table* cmd_table = (struct AHCI_Command_Table*)st->cmd_table;
    memset((void*)cmd_table, 0, sizeof(struct AHCI_Command_Table));

    int prdt_count = 0;
    if (buffer != NULL) {
        prdt_count = ahci_build_prdt(cmd_table, buffer, count * AHCI_SECTOR_SIZE);
        if (prdt_count <= 0) {
            print("AHCI Error: buffer cannot be described for DMA.\n");
            goto done;
        }
    }

    struct AHCI_Command_Header* cmd_header = ((struct AHCI_Command_Header*)st->cmd_list) + slot;
    memset((void*)cmd_header, 0, sizeof(struct AHCI_Command_Header));
    cmd_header->fis_length = 5;   // 20-byte H2D FIS in DWORDs
    cmd_header->write = write ? 1 : 0;
    cmd_header->prdt_length = (uint16_t)prdt_count;
    cmd_header->command_table_base_addr = st->cmd_table_phys;
    cmd_header->command_table_base_addr_upper = 0;

    // Host-to-device register FIS
    uint8_t* fis = cmd_table->command_fis;
    fis[0] = 0x27;    // FIS type: H2D
    fis[1] = 0x80;    // Command (not control)
    fis[2] = command;

    fis[4] = (uint8_t)(start_lba & 0xFF);
    fis[5] = (uint8_t)((start_lba >> 8) & 0xFF);
    fis[6] = (uint8_t)((start_lba >> 16) & 0xFF);
    fis[7] = 0x40;    // LBA mode

    fis[8]  = (uint8_t)((start_lba >> 24) & 0xFF);
    fis[9]  = (uint8_t)((start_lba >> 32) & 0xFF);
    fis[10] = (uint8_t)((start_lba >> 40) & 0xFF);

    fis[12] = (uint8_t)(count & 0xFF);
    fis[13] = (uint8_t)((count >> 8) & 0xFF);

    // Issue command
    port->command_issue = (1U << slot);

    timeout = complete_spins;
    while (timeout > 0) {
        if (!(port->command_issue & (1U << slot))) {
            break;
        }
        if ((port->interrupt_status & AHCI_PxIS_TFES) || (port->task_file_data & AHCI_TFD_ERR)) {
            print("AHCI Error: Task file error status detected.\n");
            goto fail;
        }
        timeout--;
    }

    if (timeout == 0) {
        print("AHCI Error: Command timed out.\n");
        goto fail;
    }

    ok = !(port->task_file_data & AHCI_TFD_ERR) && (port->sata_error == 0);
    if (ok) goto done;

fail:
    ahci_recover_port(port);

done:
    mutex_unlock(&st->lock);
    return ok;
}

static bool ahci_transfer(struct AHCI_Port_Registers* port, uint64_t start_lba, uint32_t count,
                          uint8_t* buffer, bool write) {
    if (!port || !buffer || count == 0 || count > 0xFFFF) return false;

    return ahci_execute(port, write ? ATA_CMD_WRITE_DMA_EX : ATA_CMD_READ_DMA_EX,
                        start_lba, count, buffer, write, 5000000);
}

bool ahci_flush_cache(struct AHCI_Port_Registers* port) {
    if (!port) return false;

    // Flushing a large write cache can take far longer than a single
    // sector transfer, so allow a much longer completion poll.
    return ahci_execute(port, ATA_CMD_FLUSH_CACHE_EX, 0, 0, NULL, false, 50000000);
}

bool ahci_read_sectors(struct AHCI_Port_Registers* port, uint64_t start_lba, uint32_t count, uint8_t* virtual_buffer) {
    return ahci_transfer(port, start_lba, count, virtual_buffer, false);
}

bool ahci_write_sectors(struct AHCI_Port_Registers* port, uint64_t start_lba, uint32_t count, const uint8_t* virtual_buffer) {
    return ahci_transfer(port, start_lba, count, (uint8_t*)virtual_buffer, true);
}

//
// IDENTIFY DEVICE: needed for the disk's size
//

#define ATA_CMD_IDENTIFY_DEVICE  0xEC

uint64_t ahci_identify_sector_count(struct AHCI_Port_Registers* port) {
    if (!ahci_port_ready(port)) return 0;

    uint32_t phys = 0;
    uint16_t* identify = (uint16_t*)ahci_alloc_dma_page(&phys);
    if (identify == NULL) return 0;

    uint64_t sectors = 0;

    // 512 bytes of PIO-in data, delivered by the HBA through the PRDT
    if (ahci_execute(port, ATA_CMD_IDENTIFY_DEVICE, 0, 1, (uint8_t*)identify, false, 5000000)) {
        // Word 83 bit 10: 48-bit LBA supported -> words 100-103
        if (identify[83] & (1U << 10)) {
            sectors = (uint64_t)identify[100]
                    | ((uint64_t)identify[101] << 16)
                    | ((uint64_t)identify[102] << 32)
                    | ((uint64_t)identify[103] << 48);
        }
        // Otherwise (or if that reads back zero) the LBA28 count, words 60-61
        if (sectors == 0) {
            sectors = (uint64_t)identify[60] | ((uint64_t)identify[61] << 16);
        }
    }

    vmm_free_pages(identify, 1);
    return sectors;
}

//
// Block layer adapter
//

// Sectors per command. 64 KB spans at most 17 pages, far below the PRDT
// limit even when no two pages are physically adjacent.
#define AHCI_BLOCK_CHUNK_SECTORS  128

static bool ahci_block_read(struct BlockDevice* bd, uint64_t lba, uint32_t count, void* buffer) {
    struct AHCI_Port_Registers* port = (struct AHCI_Port_Registers*)bd->driver_data;
    uint8_t* ptr = (uint8_t*)buffer;

    while (count > 0) {
        uint32_t chunk = (count < AHCI_BLOCK_CHUNK_SECTORS) ? count : AHCI_BLOCK_CHUNK_SECTORS;
        if (!ahci_read_sectors(port, lba, chunk, ptr))
            return false;
        lba   += chunk;
        count -= chunk;
        ptr   += chunk * AHCI_SECTOR_SIZE;
    }
    return true;
}

static bool ahci_block_write(struct BlockDevice* bd, uint64_t lba, uint32_t count, const void* buffer) {
    struct AHCI_Port_Registers* port = (struct AHCI_Port_Registers*)bd->driver_data;
    const uint8_t* ptr = (const uint8_t*)buffer;

    while (count > 0) {
        uint32_t chunk = (count < AHCI_BLOCK_CHUNK_SECTORS) ? count : AHCI_BLOCK_CHUNK_SECTORS;
        if (!ahci_write_sectors(port, lba, chunk, ptr))
            return false;
        lba   += chunk;
        count -= chunk;
        ptr   += chunk * AHCI_SECTOR_SIZE;
    }
    return true;
}

static bool ahci_block_flush(struct BlockDevice* bd) {
    return ahci_flush_cache((struct AHCI_Port_Registers*)bd->driver_data);
}

static const struct BlockDeviceOps ahci_block_ops = {
    .read  = ahci_block_read,
    .write = ahci_block_write,
    .flush = ahci_block_flush,
};

struct BlockDevice* ahci_create_block_device(struct AHCI_Port_Registers* port) {
    if (!ahci_port_ready(port)) return NULL;

    struct BlockDevice* existing = block_find_driver(port);
    if (existing != NULL) return existing;

    uint64_t sectors = ahci_identify_sector_count(port);
    if (sectors == 0) {
        print("AHCI Error: IDENTIFY failed, disk not registered.\n");
        return NULL;
    }

    return block_register("ahci", &ahci_block_ops, port, AHCI_SECTOR_SIZE, sectors);
}
