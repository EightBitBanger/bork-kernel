#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <kernel/arch/x86/io.h>
#include <kernel/arch/x86/drivers/acpi.h>
#include <kernel/arch/x86/drivers/serial.h>
#include <kernel/arch/x86/bus/pci/pci.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/util/string.h>

//
// ACPI power control (acpi.h)
//

#define RSDP_SIG            "RSD PTR "
#define EBDA_SEG_PTR        0x40E       // BIOS data area: the EBDA's segment
#define BIOS_AREA_START     0xE0000u
#define BIOS_AREA_END       0x100000u
#define TABLE_MAX           (1024u * 1024u)     // Longest table looked at

// FADT field offsets (ACPI 6.x, table 5.9)
#define FADT_DSDT           40
#define FADT_SMI_CMD        48
#define FADT_ACPI_ENABLE    52
#define FADT_PM1A_EVT       56
#define FADT_PM1B_EVT       60
#define FADT_PM1A_CNT       64
#define FADT_PM1B_CNT       68
#define FADT_FLAGS          112
#define FADT_RESET_REG      116     // Generic address structure, 12 bytes
#define FADT_RESET_VALUE    128
#define FADT_X_DSDT         140
#define FADT_X_PM1A_EVT     148
#define FADT_X_PM1B_EVT     160
#define FADT_X_PM1A_CNT     172
#define FADT_X_PM1B_CNT     184

#define FADT_F_RESET_REG    (1u << 10)
#define FADT_F_HW_REDUCED   (1u << 20)

#define GAS_MEMORY          0
#define GAS_IO              1
#define GAS_PCI             2

#define PM1_SCI_EN          0x0001
#define PM1_SLP_TYP_SHIFT   10
#define PM1_SLP_TYP_MASK    (7u << PM1_SLP_TYP_SHIFT)
#define PM1_SLP_EN          0x2000
#define PM1_WAK_STS         0x8000

#define AML_NAME_OP         0x08
#define AML_PACKAGE_OP      0x12
#define AML_BYTE_PREFIX     0x0A
#define AML_WORD_PREFIX     0x0B
#define AML_DWORD_PREFIX    0x0C
#define AML_ZERO            0x00
#define AML_ONE             0x01

static acpi_power_t power;
static bool         tried;

//
// Memory
//

static const uint8_t* map_phys(uint64_t phys, uint32_t len) {
    if (phys == 0 || len == 0 || phys + len > 0x100000000ULL)
        return NULL;
    return (const uint8_t*)vmm_map_mmio_region((uint32_t)phys, len, VM_PRESENT);
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

static bool checksum_ok(const uint8_t* p, uint32_t len) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++)
        sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

// A whole table (header first, for its length), checked. NULL: not there,
// too long, or a bad checksum.
static const uint8_t* map_table(uint64_t phys, const char* sig, uint32_t* out_len) {
    const uint8_t* h = map_phys(phys, 36);
    if (h == NULL || (sig != NULL && memcmp(h, sig, 4) != 0))
        return NULL;
    uint32_t len = rd32(h + 4);
    if (len < 36 || len > TABLE_MAX)
        return NULL;
    const uint8_t* t = map_phys(phys, len);
    if (t == NULL || !checksum_ok(t, len))
        return NULL;
    *out_len = len;
    return t;
}

static const uint8_t* find_rsdp_in(uint32_t start, uint32_t len) {
    const uint8_t* area = map_phys(start, len);
    if (area == NULL)
        return NULL;
    for (uint32_t off = 0; off + 20 <= len; off += 16) {
        const uint8_t* p = area + off;
        if (memcmp(p, RSDP_SIG, 8) == 0 && checksum_ok(p, 20))
            return p;
    }
    return NULL;
}

static const uint8_t* find_rsdp(void) {
    const uint8_t* bda = map_phys(0, 0x1000);
    if (bda != NULL) {
        uint32_t ebda = (uint32_t)rd16(bda + EBDA_SEG_PTR) << 4;
        if (ebda >= 0x80000 && ebda < 0xA0000) {
            const uint8_t* p = find_rsdp_in(ebda, 1024);
            if (p != NULL)
                return p;
        }
    }
    return find_rsdp_in(BIOS_AREA_START, BIOS_AREA_END - BIOS_AREA_START);
}

// The FADT ("FACP") from the XSDT (8-byte entries) or the RSDT (4-byte)
static const uint8_t* find_fadt(const uint8_t* rsdp, uint32_t* fadt_len) {
    uint8_t rev = rsdp[15];
    for (int pass = 0; pass < 2; pass++) {
        bool xsdt = (pass == 0);
        if (xsdt && rev < 2)
            continue;
        uint64_t addr = xsdt ? rd64(rsdp + 24) : rd32(rsdp + 16);
        uint32_t len;
        const uint8_t* sdt = map_table(addr, xsdt ? "XSDT" : "RSDT", &len);
        if (sdt == NULL)
            continue;
        uint32_t step = xsdt ? 8 : 4;
        for (uint32_t off = 36; off + step <= len; off += step) {
            uint64_t entry = xsdt ? rd64(sdt + off) : rd32(sdt + off);
            const uint8_t* fadt = map_table(entry, "FACP", fadt_len);
            if (fadt != NULL)
                return fadt;
        }
    }
    return NULL;
}

// A port from the FADT: the 32-bit block, else the X_ generic address when
// it is in I/O space
static uint16_t fadt_port(const uint8_t* fadt, uint32_t len, uint32_t off32, uint32_t off_x) {
    uint32_t port = rd32(fadt + off32);
    if (port == 0 && off_x + 12 <= len && fadt[off_x] == GAS_IO)
        port = (uint32_t)rd64(fadt + off_x + 4);
    return (port <= 0xFFFF) ? (uint16_t)port : 0;
}

//
// \_S5 in the DSDT's AML: Name (_S5, Package () { typa, typb, ... })
//

static bool aml_int(const uint8_t** pp, const uint8_t* end, uint8_t* out) {
    const uint8_t* p = *pp;
    if (p >= end)
        return false;
    switch (*p) {
        case AML_ZERO:          *out = 0; p += 1; break;
        case AML_ONE:           *out = 1; p += 1; break;
        case AML_BYTE_PREFIX:   if (p + 2 > end) return false; *out = p[1]; p += 2; break;
        case AML_WORD_PREFIX:   if (p + 3 > end) return false; *out = p[1]; p += 3; break;
        case AML_DWORD_PREFIX:  if (p + 5 > end) return false; *out = p[1]; p += 5; break;
        default:                return false;
    }
    *pp = p;
    return true;
}

static bool find_s5(const uint8_t* dsdt, uint32_t len, uint8_t* typa, uint8_t* typb) {
    const uint8_t* end = dsdt + len;
    for (uint32_t i = 36 + 2; i + 5 < len; i++) {
        const uint8_t* p = dsdt + i;
        if (memcmp(p, "_S5_", 4) != 0)
            continue;
        bool named = (p[-1] == AML_NAME_OP) || (p[-1] == '\\' && p[-2] == AML_NAME_OP);
        if (!named || p[4] != AML_PACKAGE_OP)
            continue;
        p += 5;
        p += ((*p >> 6) & 3) + 1;       // PkgLength
        p += 1;                         // NumElements
        uint8_t a, b;
        if (!aml_int(&p, end, &a) || !aml_int(&p, end, &b))
            continue;
        *typa = a & 7;
        *typb = b & 7;
        return true;
    }
    return false;
}

static void log_hex(const char* label, uint32_t value) {
    serial_puts(label);
    serial_put_hex(value);
}

bool acpi_init(void) {
    if (tried)
        return power.found;
    tried = true;

    const uint8_t* rsdp = find_rsdp();
    if (rsdp == NULL) {
        serial_puts("[acpi] no RSDP\n");
        return false;
    }
    uint32_t len;
    const uint8_t* fadt = find_fadt(rsdp, &len);
    if (fadt == NULL) {
        serial_puts("[acpi] no FADT\n");
        return false;
    }

    power.found     = true;
    power.revision  = fadt[8];
    power.pm1a_cnt  = fadt_port(fadt, len, FADT_PM1A_CNT, FADT_X_PM1A_CNT);
    power.pm1b_cnt  = fadt_port(fadt, len, FADT_PM1B_CNT, FADT_X_PM1B_CNT);
    power.pm1a_evt  = fadt_port(fadt, len, FADT_PM1A_EVT, FADT_X_PM1A_EVT);
    power.pm1b_evt  = fadt_port(fadt, len, FADT_PM1B_EVT, FADT_X_PM1B_EVT);
    power.smi_cmd   = (uint16_t)rd32(fadt + FADT_SMI_CMD);
    power.acpi_enable = fadt[FADT_ACPI_ENABLE];

    uint32_t flags = (len >= FADT_FLAGS + 4) ? rd32(fadt + FADT_FLAGS) : 0;
    if ((flags & FADT_F_RESET_REG) && len >= FADT_RESET_VALUE + 1) {
        power.reset_space = fadt[FADT_RESET_REG];
        power.reset_addr  = rd64(fadt + FADT_RESET_REG + 4);
        power.reset_value = fadt[FADT_RESET_VALUE];
        power.reset_reg   = (power.reset_addr != 0 && power.reset_space <= GAS_PCI);
    }

    uint64_t dsdt_addr = 0;
    if (len >= FADT_X_DSDT + 8)
        dsdt_addr = rd64(fadt + FADT_X_DSDT);
    if (dsdt_addr == 0 || dsdt_addr >= 0x100000000ULL)
        dsdt_addr = rd32(fadt + FADT_DSDT);
    uint32_t dsdt_len;
    const uint8_t* dsdt = map_table(dsdt_addr, "DSDT", &dsdt_len);
    if (dsdt != NULL)
        power.s5 = find_s5(dsdt, dsdt_len, &power.slp_typa, &power.slp_typb);

    log_hex("[acpi] FADT rev ", power.revision);
    log_hex(" PM1a_CNT ", power.pm1a_cnt);
    log_hex(" PM1b_CNT ", power.pm1b_cnt);
    log_hex(" S5 ", power.s5 ? (uint32_t)(power.slp_typa | (power.slp_typb << 8)) : 0xFFFFFFFFu);
    log_hex(" reset ", power.reset_reg ? (uint32_t)power.reset_addr : 0);
    if (flags & FADT_F_HW_REDUCED)
        serial_puts(" (hardware-reduced: no PM1 ports)");
    serial_puts("\n");
    return true;
}

const acpi_power_t* acpi_power(void) {
    acpi_init();
    return &power;
}

// About `us` microseconds: port 0x80 takes about one each
static void delay_us(uint32_t us) {
    while (us-- > 0)
        io_wait();
}

// Into ACPI mode if the firmware left it in legacy mode (SCI_EN clear): S5
// is only entered from ACPI mode on a lot of machines
static void enable_acpi_mode(void) {
    if (power.pm1a_cnt == 0 || (inw(power.pm1a_cnt) & PM1_SCI_EN))
        return;
    if (power.smi_cmd == 0 || power.acpi_enable == 0)
        return;
    outb(power.smi_cmd, power.acpi_enable);
    for (uint32_t i = 0; i < 3000; i++) {       // Up to about 3 s
        if (inw(power.pm1a_cnt) & PM1_SCI_EN)
            return;
        delay_us(1000);
    }
}

static void pm1_sleep(uint16_t cnt, uint16_t evt, uint8_t typ) {
    if (cnt == 0)
        return;
    if (evt != 0)
        outw(evt, PM1_WAK_STS);                 // Write-one-to-clear
    uint16_t v = (uint16_t)(inw(cnt) & ~(PM1_SLP_TYP_MASK | PM1_SLP_EN));
    v |= (uint16_t)(typ << PM1_SLP_TYP_SHIFT);
    outw(cnt, v);
    outw(cnt, (uint16_t)(v | PM1_SLP_EN));
}

void acpi_poweroff(void) {
    if (!acpi_init() || !power.s5 || power.pm1a_cnt == 0)
        return;
    enable_acpi_mode();
    // PM1b first: on two-block machines both writes must land, and the
    // PM1a one is the one that usually takes the power away
    pm1_sleep(power.pm1b_cnt, power.pm1b_evt, power.slp_typb);
    pm1_sleep(power.pm1a_cnt, power.pm1a_evt, power.slp_typa);
    delay_us(100000);                           // Give it time to go
}

void acpi_reset(void) {
    if (!acpi_init() || !power.reset_reg)
        return;
    switch (power.reset_space) {
        case GAS_IO:
            if (power.reset_addr <= 0xFFFF)
                outb((uint16_t)power.reset_addr, power.reset_value);
            break;
        case GAS_MEMORY: {
            volatile uint8_t* reg = (volatile uint8_t*)vmm_map_mmio_region(
                (uint32_t)power.reset_addr, 1, VM_PRESENT | VM_READWRITE | VM_PCD);
            if (reg != NULL)
                *reg = power.reset_value;
            break;
        }
        case GAS_PCI:   // Bus 0; device in bits 32-47, function 16-31, offset 0-15
            pci_config_write8(0, (uint8_t)(power.reset_addr >> 32), (uint8_t)(power.reset_addr >> 16),
                              (uint8_t)power.reset_addr, power.reset_value);
            break;
    }
    delay_us(50000);
}
