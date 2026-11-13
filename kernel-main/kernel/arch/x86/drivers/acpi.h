#ifndef _X86_DRIVERS_ACPI_H_
#define _X86_DRIVERS_ACPI_H_

#include <stdint.h>
#include <stdbool.h>

//
// ACPI power control: just enough of the tables to turn the machine off and
// to reset it (system_poweroff / system_restart, util/system.h).
//
// acpi_init() finds the RSDP (EBDA, then 0xE0000 - 0xFFFFF: BIOS boot),
// the RSDT or XSDT, the FADT and the DSDT, and reads \_S5's sleep type out
// of the DSDT's AML. It runs once, the first time anything below is called
// (or at boot); later calls return what it found. Tables are mapped with
// vmm_map_mmio_region() and stay mapped.
//

typedef struct {
    bool     found;             // A FADT was found
    bool     s5;                // \_S5 was found in the DSDT
    uint8_t  revision;          // FADT revision
    uint16_t pm1a_cnt;          // PM1 control ports (0: none)
    uint16_t pm1b_cnt;
    uint16_t pm1a_evt;          // PM1 event (status) ports
    uint16_t pm1b_evt;
    uint16_t smi_cmd;           // SMI command port, ACPI_ENABLE value
    uint8_t  acpi_enable;
    uint8_t  slp_typa;          // \_S5 sleep types
    uint8_t  slp_typb;
    bool     reset_reg;         // FADT RESET_REG_SUP: the reset register works
    uint8_t  reset_space;       // 0 memory, 1 I/O, 2 PCI configuration
    uint64_t reset_addr;
    uint8_t  reset_value;
} acpi_power_t;

// Find the tables (once). True when a FADT was found.
bool acpi_init(void);

// What acpi_init() found
const acpi_power_t* acpi_power(void);

// Enter S5 (soft off). Interrupts must be off. Returns only if the machine
// is still on afterwards (no FADT, no \_S5, or the firmware ignored it).
void acpi_poweroff(void);

// Write the FADT reset register. Returns if there is none, or nothing
// happened.
void acpi_reset(void);

#endif
