#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "kshim.h"
#include "ar9485.h"
#include "ar9485_regs.h"
#include "ar9485_tables.h"

#include <kernel/arch/x86/drivers/net/wlan.h>
#include <kernel/arch/x86/drivers/net/wlan_scan.h>

//
// Atheros AR9485 (see ar9485.h)
//
// Ported from OpenBSD's athn(4): athn.c, ar9003.c and ar9380.c (the
// AR9380 / AR9485 routines). The PLL set-up follows Linux's ath9k
// (hw.c, ath9k_hw_init_pll), which programs the AR9485's baseband PLL
// where athn does not. Both are under this licence:
//
/*-
 * Copyright (c) 2009-2010 Damien Bergamini <damien.bergamini@free.fr>
 * Copyright (c) 2008-2011 Atheros Communications Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
//
// What athn does that this leaves out: TX IQ and PA calibration, spur
// mitigation, the hardware key cache (the station encrypts in software),
// rate control, power saving, beacons and interrupts (the driver polls).
// One transmit queue, legacy (11b/g) rates only.
//

#define MS(v, f)        (((v) & f##_M) >> f##_S)
#define SM(f, v)        (((uint32_t)(v) << f##_S) & f##_M)
#define RW(var, f, v)   (((var) & ~f##_M) | SM(f, v))

// Registers in the tables are stored divided by 4
#define X(x)            ((uint32_t)(x) << 2)

// Analog registers need time to settle after a write
#define AR_IS_ANALOG_REG(reg)   ((reg) >= 0x7800 && (reg) <= 0x78b4)

// Default noise floor: the combined RSSI is relative to it
#define AR9485_NOISE_FLOOR      (-95)
#define AR9485_RSSI_BAD         (-128)

#define AR9485_MAX_GPIO         17

//
// Register access
//

static uint32_t rd(struct Ar9485* c, uint32_t reg) {
    return c->bus.read(c->bus.ctx, reg);
}

static void wr(struct Ar9485* c, uint32_t reg, uint32_t value) {
    c->bus.write(c->bus.ctx, reg, value);
}

static void setbits(struct Ar9485* c, uint32_t reg, uint32_t bits) {
    wr(c, reg, rd(c, reg) | bits);
}

static void clrbits(struct Ar9485* c, uint32_t reg, uint32_t bits) {
    wr(c, reg, rd(c, reg) & ~bits);
}

static void delay(struct Ar9485* c, uint32_t us) {
    c->bus.delay_us(us);
}

// Poll until (reg & mask) == value, `tries` times `us` apart
static bool wait_for(struct Ar9485* c, uint32_t reg, uint32_t mask, uint32_t value, uint32_t tries, uint32_t us) {
    for (uint32_t i = 0; i < tries; i++) {
        if ((rd(c, reg) & mask) == value)
            return true;
        delay(c, us);
    }
    return false;
}

static bool fail(struct Ar9485* c, const char* why) {
    c->error = why;
    return false;
}

//
// Identification, reset and power (athn.c)
//

bool ar9485_identify(struct Ar9485* c) {
    uint32_t reg = rd(c, AR_SREV);
    c->srev = reg;
    if (reg == 0xFFFFFFFFU || reg == 0)
        return fail(c, "the card does not answer");

    if (MS(reg, AR_SREV_ID) == 0xff) {
        c->mac_ver = (uint16_t)MS(reg, AR_SREV_VERSION2);
        c->mac_rev = (uint8_t)MS(reg, AR_SREV_REVISION2);
    } else {
        c->mac_ver = (uint16_t)MS(reg, AR_SREV_VERSION);
        c->mac_rev = (uint8_t)MS(reg, AR_SREV_REVISION);
    }
    if (c->mac_ver != AR_SREV_VERSION_9485)
        return fail(c, "not an AR9485");

    // ath9k keeps a copy of this with two PCIe power-saving workarounds
    // set and writes it back before every reset (it can't be read while
    // the chip sleeps)
    c->wa = rd(c, AR_WA) | AR_WA_D3_L1_DISABLE | AR_WA_ASPM_TIMER_BASED_DISABLE;
    return true;
}

static bool ar9485_reset(struct Ar9485* c, bool cold) {
    wr(c, AR_WA, c->wa);
    delay(c, 10);

    wr(c, AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN | AR_RTC_FORCE_WAKE_ON_INT);

    if (rd(c, AR_INTR_SYNC_CAUSE) & (AR_INTR_SYNC_LOCAL_TIMEOUT | AR_INTR_SYNC_RADM_CPL_TIMEOUT)) {
        wr(c, AR_INTR_SYNC_ENABLE, 0);
        wr(c, AR_RC, AR_RC_HOSTIF);
    }

    wr(c, AR_RTC_RC, AR_RTC_RC_MAC_WARM | (cold ? AR_RTC_RC_MAC_COLD : 0));
    delay(c, 50);
    wr(c, AR_RTC_RC, 0);
    if (!wait_for(c, AR_RTC_RC, AR_RTC_RC_MAC_WARM | AR_RTC_RC_MAC_COLD, 0, 1000, 10))
        return fail(c, "stuck in MAC reset");
    wr(c, AR_RC, 0);
    return true;
}

static bool ar9485_reset_power_on(struct Ar9485* c) {
    wr(c, AR_WA, c->wa);
    delay(c, 10);

    wr(c, AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN | AR_RTC_FORCE_WAKE_ON_INT);
    // RTC reset and clear
    wr(c, AR_RTC_RESET, 0);
    delay(c, 2);
    wr(c, AR_RTC_RESET, 1);

    if (!wait_for(c, AR_RTC_STATUS, AR_RTC_STATUS_M, AR_RTC_STATUS_ON, 1000, 10))
        return fail(c, "RTC did not wake after a power-on reset");
    return ar9485_reset(c, false);
}

static bool ar9485_set_power_awake(struct Ar9485* c) {
    if ((rd(c, AR_RTC_STATUS) & AR_RTC_STATUS_M) == AR_RTC_STATUS_SHUTDOWN) {
        if (!ar9485_reset_power_on(c))
            return false;
    }
    setbits(c, AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN);
    delay(c, 50);

    uint32_t tries = 0;
    while ((rd(c, AR_RTC_STATUS) & AR_RTC_STATUS_M) != AR_RTC_STATUS_ON) {
        if (++tries >= 4000)
            return fail(c, "RTC did not wake");
        delay(c, 50);
        setbits(c, AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN);
    }
    clrbits(c, AR_STA_ID1, AR_STA_ID1_PWR_SAV);
    return true;
}

static void ar9485_set_power_sleep(struct Ar9485* c) {
    setbits(c, AR_STA_ID1, AR_STA_ID1_PWR_SAV);
    // Allow the MAC to go to sleep
    clrbits(c, AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN);
    clrbits(c, AR_RTC_RESET, AR_RTC_RESET_EN);
}

// ath9k_hw_init_pll for the AR9485: the baseband PLL first (ki 0x4,
// kd 0x40, refdiv 5, nint 0x58), then the RTC PLL (refdiv 5, div 0x2c)
static void ar9485_init_pll(struct Ar9485* c) {
    uint32_t reg;
    reg = rd(c, AR_CH0_BB_DPLL2);
    reg = RW(reg, AR_CH0_BB_DPLL2_PLL_PWD, 1);
    reg = RW(reg, AR_CH0_DPLL2_KD, 0x40);
    reg = RW(reg, AR_CH0_DPLL2_KI, 0x4);
    wr(c, AR_CH0_BB_DPLL2, reg);

    reg = rd(c, AR_CH0_BB_DPLL1);
    reg = RW(reg, AR_CH0_BB_DPLL1_REFDIV, 0x5);
    reg = RW(reg, AR_CH0_BB_DPLL1_NINI, 0x58);
    reg = RW(reg, AR_CH0_BB_DPLL1_NFRAC, 0x0);
    wr(c, AR_CH0_BB_DPLL1, reg);

    reg = rd(c, AR_CH0_BB_DPLL2);
    reg = RW(reg, AR_CH0_BB_DPLL2_OUTDIV, 0x1);
    reg = RW(reg, AR_CH0_BB_DPLL2_LOCAL_PLL, 0x1);
    reg = RW(reg, AR_CH0_BB_DPLL2_EN_NEGTRIG, 0x1);
    wr(c, AR_CH0_BB_DPLL2, reg);

    reg = rd(c, AR_CH0_BB_DPLL3);
    reg = RW(reg, AR_CH0_BB_DPLL3_PHASE_SHIFT, 0x6);
    wr(c, AR_CH0_BB_DPLL3, reg);

    reg = rd(c, AR_CH0_BB_DPLL2);
    reg = RW(reg, AR_CH0_BB_DPLL2_PLL_PWD, 0);
    wr(c, AR_CH0_BB_DPLL2, reg);
    delay(c, 1000);

    wr(c, AR_RTC_PLL_CONTROL, SM(AR_RTC_9300_PLL_REFDIV, 0x5) | SM(AR_RTC_9300_PLL_DIV, 0x2c));
    delay(c, 1000);
    delay(c, AR_RTC_PLL_SETTLE_DELAY);
    wr(c, AR_RTC_SLEEP_CLK, AR_RTC_FORCE_DERIVED_CLK);
}

// PCIe: let the link sleep in L0s / L1 as the card's serdes table says
static void ar9485_config_pcie(struct Ar9485* c) {
    for (uint32_t i = 0; i < AR_NITEMS(ar9485_1_1_serdes_regs); i++)
        wr(c, ar9485_1_1_serdes_regs[i], ar9485_1_1_serdes_vals[i]);
    delay(c, 1000);
    setbits(c, AR_PCIE_PM_CTRL, AR_PCIE_PM_CTRL_ENA);
    wr(c, AR_WA, c->wa);
}

//
// ROM (ar9003.c): an EEPROM or OTP memory, read from its top down in
// blocks, each a header, data and a checksum. The ROM is the card's say-so,
// so every address and length is checked before it is used.
//

static bool rom_eep_word(struct Ar9485* c, uint32_t addr, uint16_t* out) {
    (void)rd(c, AR_EEPROM_OFFSET(addr));
    for (uint32_t i = 0; i < 1000; i++) {
        uint32_t reg = rd(c, AR_EEPROM_STATUS_DATA);
        if (!(reg & (AR_EEPROM_STATUS_DATA_BUSY | AR_EEPROM_STATUS_DATA_PROT_ACCESS))) {
            *out = (uint16_t)MS(reg, AR_EEPROM_STATUS_DATA_VAL);
            return true;
        }
        delay(c, 10);
    }
    return false;
}

// The EEPROM is a little-endian stream of 16-bit words: byte b is the
// high half of word b / 2 when b is odd, the low half when even. Blocks
// are read from the top down, so `len` bytes from `addr` downwards
// (addr >= len - 1, checked by the caller). This is ath9k's
// ar9300_read_eeprom; athn reads the odd first byte from the wrong half.
static bool rom_eep_byte(struct Ar9485* c, uint32_t addr, uint8_t* out) {
    uint16_t val;
    if (!rom_eep_word(c, addr / 2, &val))
        return false;
    *out = (uint8_t)(val >> (8 * (addr % 2)));
    return true;
}

static bool rom_eep_data(struct Ar9485* c, uint32_t addr, uint8_t* dst, uint32_t len) {
    if (len > 0 && addr % 2 == 0) {
        if (!rom_eep_byte(c, addr, dst++))
            return false;
        addr--;
        len--;
    }
    for (; len >= 2; addr -= 2, len -= 2) {
        uint16_t val;
        if (!rom_eep_word(c, addr / 2, &val))
            return false;
        *dst++ = (uint8_t)(val >> 8);
        *dst++ = (uint8_t)(val & 0xff);
    }
    if (len > 0 && !rom_eep_byte(c, addr, dst))
        return false;
    return true;
}

static bool rom_otp_word(struct Ar9485* c, uint32_t addr, uint32_t* out) {
    (void)rd(c, AR_OTP_BASE(addr));
    for (uint32_t i = 0; i < 1000; i++) {
        if (MS(rd(c, AR_OTP_STATUS), AR_OTP_STATUS_TYPE) == AR_OTP_STATUS_VALID) {
            *out = rd(c, AR_OTP_READ_DATA);
            return true;
        }
        delay(c, 10);
    }
    return false;
}

static bool rom_otp_data(struct Ar9485* c, uint32_t addr, uint8_t* dst, uint32_t len) {
    for (; len > 0; addr--, len--) {
        uint32_t val;
        if (!rom_otp_word(c, addr >> 2, &val))
            return false;
        *dst++ = (uint8_t)(val >> ((addr & 3) * 8));
    }
    return true;
}

// `len` bytes ending at `addr` (reading downwards). False for a range
// that would run below address 0 or past the ROM.
static bool rom_read(struct Ar9485* c, int32_t addr, void* dst, uint32_t len) {
    if (addr < 0 || addr >= (int32_t)c->rom_size || (uint32_t)addr + 1 < len)
        return false;
    return c->rom_otp ? rom_otp_data(c, (uint32_t)addr, (uint8_t*)dst, len)
                      : rom_eep_data(c, (uint32_t)addr, (uint8_t*)dst, len);
}

static bool rom_header_ok(uint32_t hdr) {
    return hdr != 0 && hdr != 0xFFFFFFFFU;
}

// Where the ROM is: an EEPROM of 4 KB (AR9485), 1 KB or 512 bytes, or OTP
// of 1 KB or 512 bytes. The first with a sane header at its top wins (the
// order ath9k tries them in).
static bool rom_find(struct Ar9485* c) {
    static const struct { bool otp; uint16_t size; } where[] = {
        { false, 4096 }, { false, 1024 }, { false, 512 }, { true, 1024 }, { true, 512 },
    };
    for (uint32_t i = 0; i < AR_NITEMS(where); i++) {
        c->rom_otp  = where[i].otp;
        c->rom_size = where[i].size;
        uint32_t hdr = 0;
        if (rom_read(c, c->rom_size - 1, &hdr, sizeof(hdr)) && rom_header_ok(hdr))
            return true;
    }
    return false;
}

static const uint8_t* rom_template(uint8_t ref) {
    for (uint32_t i = 0; i < AR_NITEMS(ar9380_rom_templates); i++) {
        if (ar9380_rom_templates[i][1] == ref)
            return ar9380_rom_templates[i];
    }
    return NULL;
}

// One block into the image. False (image untouched by the bad part) for a
// template we don't have or a chunk that would land outside the image.
static bool rom_restore_block(uint8_t* eep, uint8_t alg, uint8_t ref, const uint8_t* buf, uint32_t len) {
    if (alg == AR_EEP_COMPRESS_BLOCK) {
        // Chunks that shadow a template (ref 0: the image as it stands)
        static uint8_t work[AR9485_EEP_IMAGE_SIZE];
        if (ref != 0) {
            const uint8_t* def = rom_template(ref);
            if (def == NULL)
                return false;
            memcpy(work, def, AR9485_EEP_IMAGE_SIZE);
        } else {
            memcpy(work, eep, AR9485_EEP_IMAGE_SIZE);
        }
        uint32_t off = 0;
        uint32_t pos = 0;
        while (pos + 2 <= len) {
            off += buf[pos++];              // Gap after the previous chunk
            uint32_t clen = buf[pos++];
            if (clen > len - pos || off > AR9485_EEP_IMAGE_SIZE || clen > AR9485_EEP_IMAGE_SIZE - off)
                return false;
            memcpy(&work[off], &buf[pos], clen);
            pos += clen;
            off += clen;
        }
        memcpy(eep, work, AR9485_EEP_IMAGE_SIZE);
        return true;
    }
    if (alg == AR_EEP_COMPRESS_NONE) {
        if (len != AR9485_EEP_IMAGE_SIZE)
            return false;
        memcpy(eep, buf, len);
        return true;
    }
    return false;                           // LZMA / pairs: never seen on these cards
}

static bool rom_load(struct Ar9485* c) {
    static uint8_t buf[AR9485_EEP_IMAGE_SIZE];
    // Linux starts from its default template too: blocks only patch it
    memcpy(c->eep, ar9380_def_rom, AR9485_EEP_IMAGE_SIZE);
    c->rom_blocks = 0;

    int32_t addr = (int32_t)c->rom_size - 1;
    for (uint32_t i = 0; i < 100; i++) {
        uint32_t hdr;
        if (!rom_read(c, addr, &hdr, sizeof(hdr)) || !rom_header_ok(hdr))
            break;
        addr -= (int32_t)sizeof(hdr);

        const uint8_t* h = (const uint8_t*)&hdr;
        uint8_t  alg = (uint8_t)((h[0] & 0xe0) >> 5);
        uint8_t  ref = (uint8_t)((h[1] & 0x80) >> 2 | (h[0] & 0x1f));
        uint32_t len = (uint32_t)(h[1] & 0x7f) << 4 | (uint32_t)(h[2] & 0xf0) >> 4;    // <= 2047

        // ath9k's limit for the AR9485: no block is bigger than the image.
        // A header that says otherwise is skipped, not followed.
        if (len == 0 || len > AR9485_EEP_IMAGE_SIZE)
            continue;
        if (!rom_read(c, addr, buf, len))
            break;
        addr -= (int32_t)len;

        uint16_t sum;
        if (!rom_read(c, addr, &sum, sizeof(sum)))
            break;
        addr -= (int32_t)sizeof(sum);

        uint16_t rsum = 0;
        for (uint32_t j = 0; j < len; j++)
            rsum = (uint16_t)(rsum + buf[j]);
        if (sum != rsum)
            continue;                       // Bad block: skipped, as athn and ath9k do
        if (rom_restore_block(c->eep, alg, ref, buf, len))
            c->rom_blocks++;
    }
    return c->rom_blocks > 0;
}

// A real station address: not all zeros, not all ones, not multicast
static bool mac_usable(const uint8_t* mac) {
    bool zero = true, ones = true;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0x00) zero = false;
        if (mac[i] != 0xFF) ones = false;
    }
    return !zero && !ones && !(mac[0] & 0x01);
}

static void rom_setup(struct Ar9485* c) {
    const struct ar9380_eeprom* eep = (const struct ar9380_eeprom*)c->eep;
    const struct ar9380_base_eep_hdr* base = &eep->baseEepHeader;

    memcpy(c->mac, eep->macAddr, 6);
    c->mac_valid = c->rom_ok && mac_usable(c->mac);

    // One chain on this chip, whatever the ROM claims
    c->txmask = (uint8_t)(MS(base->txrxMask, AR_EEP_TX_MASK) & 0x1);
    c->rxmask = (uint8_t)(MS(base->txrxMask, AR_EEP_RX_MASK) & 0x1);
    if (c->txmask == 0) c->txmask = 1;
    if (c->rxmask == 0) c->rxmask = 1;

    c->rfsilent = false;
    if (base->rfSilent & AR_EEP_RFSILENT_ENABLED) {
        uint8_t pin = (uint8_t)MS(base->rfSilent, AR_EEP_RFSILENT_GPIO_SEL);
        if (pin < AR9485_MAX_GPIO) {
            c->rfsilent = true;
            c->rfsilent_pin = pin;
            c->rfsilent_reversed = (base->rfSilent & AR_EEP_RFSILENT_POLARITY) != 0;
        }
    }
}

bool ar9485_attach(struct Ar9485* c, uint8_t* eep) {
    c->eep = eep;
    c->rx_on = false;
    if (!ar9485_reset_power_on(c))
        return false;
    if (!ar9485_set_power_awake(c))
        return false;
    ar9485_config_pcie(c);

    c->rom_ok = rom_find(c) && rom_load(c);
    if (!c->rom_ok) {
        // Template values: enough to receive with, but no MAC address
        memcpy(c->eep, ar9380_def_rom, AR9485_EEP_IMAGE_SIZE);
        c->rom_size = 0;
    }
    rom_setup(c);

    ar9485_set_power_sleep(c);
    return true;
}

//
// GPIO: the wireless switch (ar9003.c)
//

static bool ar9485_radio_switched_off(struct Ar9485* c) {
    if (!c->rfsilent)
        return false;
    uint32_t pin = c->rfsilent_pin;
    uint32_t reg = rd(c, AR_GPIO_OE_OUT);
    reg &= ~(AR_GPIO_OE_OUT_DRV_M << (pin * 2));
    reg |= AR_GPIO_OE_OUT_DRV_NO << (pin * 2);
    wr(c, AR_GPIO_OE_OUT, reg);

    bool level = ((rd(c, AR_GPIO_IN) & AR9300_GPIO_IN_VAL) & (1U << pin)) != 0;
    if (c->rfsilent_reversed)
        level = !level;
    return !level;
}

//
// PHY set-up for one channel (ar9003.c, ar9380.c)
//

static uint32_t ar9485_synth_delay(struct Ar9485* c) {
    return MS(rd(c, AR_PHY_RX_DELAY), AR_PHY_RX_DELAY_DELAY) / 10;    // In 100 ns steps
}

static void ar9485_set_phy(struct Ar9485* c) {
    uint32_t phy = rd(c, AR_PHY_GEN_CTRL);
    phy |= AR_PHY_GC_HT_EN | AR_PHY_GC_SHORT_GI_40 | AR_PHY_GC_SINGLE_HT_LTF1 | AR_PHY_GC_WALSH;
    phy &= ~AR_PHY_GC_GF_DETECT_EN;
    wr(c, AR_PHY_GEN_CTRL, phy);
    wr(c, AR_2040_MODE, 0);
    wr(c, AR_GTXTO, SM(AR_GTXTO_TIMEOUT_LIMIT, 25));
    wr(c, AR_CST, SM(AR_CST_TIMEOUT_LIMIT, 15));
}

static void ar9485_init_chains(struct Ar9485* c) {
    wr(c, AR_PHY_RX_CHAINMASK, c->rxmask);
    wr(c, AR_PHY_CAL_CHAINMASK, c->rxmask);
    wr(c, AR_SELFGEN_MASK, c->txmask);
}

static void ar9485_write_table(struct Ar9485* c, const uint16_t* regs, const uint32_t* vals, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        wr(c, X(regs[i]), vals[i]);
        if (AR_IS_ANALOG_REG(X(regs[i])))
            delay(c, 100);
        if ((i & 0x1f) == 0)
            delay(c, 1);
    }
}

static void ar9485_hw_init(struct Ar9485* c) {
    // Pre and core phases, then the 2.4 GHz HT20 post phase
    ar9485_write_table(c, ar9485_1_1_cm_regs, ar9485_1_1_cm_vals, AR_NITEMS(ar9485_1_1_cm_regs));
    ar9485_write_table(c, ar9485_1_1_regs, ar9485_1_1_vals_2g20, AR_NITEMS(ar9485_1_1_regs));
    ar9485_write_table(c, ar9485_1_1_rx_gain_regs, ar9485_1_1_rx_gain_vals, AR_NITEMS(ar9485_1_1_rx_gain_regs));
    // The gain table only: nothing here ever sets a transmit power
    ar9485_write_table(c, ar9485_1_1_tx_gain_regs, ar9485_1_1_tx_gain_vals_2g, AR_NITEMS(ar9485_1_1_tx_gain_regs));

    // No receiving until rx_start: frames now would carry corrupt status
    setbits(c, AR_DIAG_SW, AR_DIAG_RX_DIS | AR_DIAG_RX_ABORT);

    uint32_t reg = rd(c, AR_PCU_MISC_MODE2);
    reg &= ~AR_PCU_MISC_MODE2_ADHOC_MCAST_KEYID_ENABLE;
    reg |= AR_PCU_MISC_MODE2_AGG_WEP_ENABLE_FIX | AR_PCU_MISC_MODE2_ENABLE_AGGWEP;
    wr(c, AR_PCU_MISC_MODE2, reg);

    ar9485_set_phy(c);
    ar9485_init_chains(c);
}

static void ar9485_delta_slope_coeff(uint32_t coeff, uint32_t* exponent, uint32_t* mantissa) {
    // exponent = 14 - floor(log2(coeff)), mantissa = floor(coeff * 2^exponent + 0.5)
    uint32_t exp;
    for (exp = 31; exp > 0; exp--) {
        if (coeff & (1U << exp))
            break;
    }
    exp = 14 - (exp - 24);
    uint32_t man = coeff + (1U << (24 - exp - 1));
    *mantissa = man >> (24 - exp);
    *exponent = exp - 16;
}

static void ar9485_set_delta_slope(struct Ar9485* c, uint32_t freq) {
    uint32_t coeff = (100U << 24) / freq;
    uint32_t exp, man;
    ar9485_delta_slope_coeff(coeff, &exp, &man);
    uint32_t reg = rd(c, AR_PHY_TIMING3);
    reg = RW(reg, AR_PHY_TIMING3_DSC_EXP, exp);
    reg = RW(reg, AR_PHY_TIMING3_DSC_MAN, man);
    wr(c, AR_PHY_TIMING3, reg);

    // Short guard interval: 9/10 of it
    ar9485_delta_slope_coeff((9 * coeff) / 10, &exp, &man);
    reg = rd(c, AR_PHY_SGI_DELTA);
    reg = RW(reg, AR_PHY_SGI_DSC_EXP, exp);
    reg = RW(reg, AR_PHY_SGI_DSC_MAN, man);
    wr(c, AR_PHY_SGI_DELTA, reg);
}

static void pmu_write(struct Ar9485* c, uint32_t addr, uint32_t val) {
    wr(c, addr, val);
    for (uint32_t i = 0; i < 100; i++) {
        if (rd(c, addr) == val)
            return;
        wr(c, addr, val);       // Insist
        delay(c, 10);
    }
}

// Switching regulator (ar9485_init_swreg)
static void ar9485_init_swreg(struct Ar9485* c, const struct ar9380_eeprom* eep) {
    pmu_write(c, AR_PHY_PMU2, rd(c, AR_PHY_PMU2) & ~AR_PHY_PMU2_PGM);
    if (eep->baseEepHeader.featureEnable & AR_EEP_INTERNAL_REGULATOR) {
        pmu_write(c, AR_PHY_PMU1, 0x131dc17a);
        uint32_t reg = rd(c, AR_PHY_PMU2);
        reg = (reg & ~0xffc00000U) | 0x10000000U;
        pmu_write(c, AR_PHY_PMU2, reg);
    } else {
        pmu_write(c, AR_PHY_PMU1, rd(c, AR_PHY_PMU1) | AR_PHY_PMU1_PWD);
    }
    pmu_write(c, AR_PHY_PMU2, rd(c, AR_PHY_PMU2) | AR_PHY_PMU2_PGM);
}

// What the ROM says about the antenna, its switch and the crystal
// (ar9380_init_from_rom, 2.4 GHz, one chain)
static void ar9485_init_from_rom(struct Ar9485* c) {
    const struct ar9380_eeprom* eep = (const struct ar9380_eeprom*)c->eep;
    const struct ar9380_modal_eep_header* modal = &eep->modalHeader2G;
    uint32_t reg;

    reg = rd(c, AR9485_PHY_65NM_CH0_TOP2);
    reg = RW(reg, AR9485_PHY_65NM_CH0_TOP2_XPABIASLVL, modal->xpaBiasLvl);
    wr(c, AR9485_PHY_65NM_CH0_TOP2, reg);

    reg = rd(c, AR_PHY_SWITCH_COM);
    reg = RW(reg, AR_SWITCH_TABLE_COM_ALL, modal->antCtrlCommon);
    wr(c, AR_PHY_SWITCH_COM, reg);
    reg = rd(c, AR_PHY_SWITCH_COM_2);
    reg = RW(reg, AR_SWITCH_TABLE_COM_2_ALL, modal->antCtrlCommon2);
    wr(c, AR_PHY_SWITCH_COM_2, reg);
    reg = rd(c, AR_PHY_SWITCH_CHAIN(0));
    reg = RW(reg, AR_SWITCH_TABLE_ALL, modal->antCtrlChain[0]);
    wr(c, AR_PHY_SWITCH_CHAIN(0), reg);

    uint8_t ant_div = eep->base_ext1.ant_div_control;
    reg = rd(c, AR_PHY_MC_GAIN_CTRL);
    reg = RW(reg, AR_PHY_MC_GAIN_CTRL_ANT_DIV_CTRL_ALL, MS(ant_div, AR_EEP_ANT_DIV_CTRL_ALL));
    if (ant_div & AR_EEP_ANT_DIV_CTRL_ANT_DIV)
        reg |= AR_PHY_MC_GAIN_CTRL_ENABLE_ANT_DIV;
    else
        reg &= ~AR_PHY_MC_GAIN_CTRL_ENABLE_ANT_DIV;
    wr(c, AR_PHY_MC_GAIN_CTRL, reg);
    reg = rd(c, AR_PHY_CCK_DETECT);
    if (ant_div & AR_EEP_ANT_DIV_CTRL_FAST_DIV)
        reg |= AR_PHY_CCK_DETECT_BB_ENABLE_ANT_FAST_DIV;
    else
        reg &= ~AR_PHY_CCK_DETECT_BB_ENABLE_ANT_FAST_DIV;
    wr(c, AR_PHY_CCK_DETECT, reg);

    if (eep->baseEepHeader.miscConfiguration & AR_EEP_DRIVE_STRENGTH) {
        reg = rd(c, AR_PHY_65NM_CH0_BIAS1);
        reg = RW(reg, AR_PHY_65NM_CH0_BIAS1_0, 5);
        reg = RW(reg, AR_PHY_65NM_CH0_BIAS1_1, 5);
        reg = RW(reg, AR_PHY_65NM_CH0_BIAS1_2, 5);
        reg = RW(reg, AR_PHY_65NM_CH0_BIAS1_3, 5);
        reg = RW(reg, AR_PHY_65NM_CH0_BIAS1_4, 5);
        reg = RW(reg, AR_PHY_65NM_CH0_BIAS1_5, 5);
        wr(c, AR_PHY_65NM_CH0_BIAS1, reg);

        reg = rd(c, AR_PHY_65NM_CH0_BIAS2);
        reg = RW(reg, AR_PHY_65NM_CH0_BIAS2_0, 5);
        reg = RW(reg, AR_PHY_65NM_CH0_BIAS2_1, 5);
        reg = RW(reg, AR_PHY_65NM_CH0_BIAS2_2, 5);
        reg = RW(reg, AR_PHY_65NM_CH0_BIAS2_3, 5);
        reg = RW(reg, AR_PHY_65NM_CH0_BIAS2_4, 5);
        reg = RW(reg, AR_PHY_65NM_CH0_BIAS2_5, 5);
        reg = RW(reg, AR_PHY_65NM_CH0_BIAS2_6, 5);
        reg = RW(reg, AR_PHY_65NM_CH0_BIAS2_7, 5);
        reg = RW(reg, AR_PHY_65NM_CH0_BIAS2_8, 5);
        wr(c, AR_PHY_65NM_CH0_BIAS2, reg);

        reg = rd(c, AR_PHY_65NM_CH0_BIAS4);
        reg = RW(reg, AR_PHY_65NM_CH0_BIAS4_0, 5);
        reg = RW(reg, AR_PHY_65NM_CH0_BIAS4_1, 5);
        reg = RW(reg, AR_PHY_65NM_CH0_BIAS4_2, 5);
        wr(c, AR_PHY_65NM_CH0_BIAS4, reg);
    }

    reg = rd(c, AR_PHY_EXT_ATTEN_CTL(0));
    reg = RW(reg, AR_PHY_EXT_ATTEN_CTL_XATTEN1_DB, modal->xatten1DB[0]);
    reg = RW(reg, AR_PHY_EXT_ATTEN_CTL_XATTEN1_MARGIN, modal->xatten1Margin[0]);
    wr(c, AR_PHY_EXT_ATTEN_CTL(0), reg);

    ar9485_init_swreg(c, eep);

    if (eep->baseEepHeader.featureEnable & AR_EEP_TUNING_CAPS) {
        reg = rd(c, AR9485_PHY_CH0_XTAL);
        reg = RW(reg, AR9485_PHY_CH0_XTAL_CAPINDAC, eep->baseEepHeader.params_for_tuning_caps[0]);
        reg = RW(reg, AR9485_PHY_CH0_XTAL_CAPOUTDAC, eep->baseEepHeader.params_for_tuning_caps[0]);
        wr(c, AR9485_PHY_CH0_XTAL, reg);
    }
}

//
// Transmit power (ar9380_set_txpower, ar9380_set_correction): the ROM's
// target powers for this channel, per rate, and its open loop power
// control correction. One chain, 2.4 GHz, 20 MHz.
//

static int32_t interpolate(int32_t x, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    if (x1 == x2)
        return y1;
    return y1 + ((x - x1) * (y2 - y1)) / (x2 - x1);
}

// The ROM's samples bracketing `fbin` (2.4 GHz: MHz - 2300): the largest
// at or below it and the smallest at or above it, or the nearest one
static void target_pick(uint8_t fbin, const uint8_t* fbins, uint32_t count, uint32_t* lo, uint32_t* hi) {
    int32_t l = -1, h = -1;
    for (uint32_t i = 0; i < count; i++) {
        int32_t delta = (int32_t)fbin - fbins[i];
        if (delta >= 0 && (l < 0 || delta < (int32_t)fbin - fbins[l]))
            l = (int32_t)i;
        if (delta <= 0 && (h < 0 || delta > (int32_t)fbin - fbins[h]))
            h = (int32_t)i;
    }
    if (l < 0)
        l = h;
    else if (h < 0)
        h = l;
    *lo = (uint32_t)l;
    *hi = (uint32_t)h;
}

// The ROM's word for one rate group, at most the ceiling, never below 0
static int16_t target_power(uint8_t fbin, const uint8_t* fbins, uint32_t lo, uint32_t hi,
                            const uint8_t* plo, const uint8_t* phi) {
    int32_t p = interpolate(fbin, fbins[lo], *plo, fbins[hi], *phi);
    if (p > AR9485_TXPOWER_MAX)
        p = AR9485_TXPOWER_MAX;
    if (p < 0)
        p = 0;
    return (int16_t)p;
}

#define POW(p, shift)   (((uint32_t)(p) & 0x3f) << (shift))

static void ar9485_set_txpower(struct Ar9485* c, uint32_t freq) {
    const struct ar9380_eeprom* eep = (const struct ar9380_eeprom*)c->eep;
    uint8_t fbin = (uint8_t)(freq - 2300);
    uint32_t lo, hi;

    int16_t cck[4], ofdm[4], ht[14];
    target_pick(fbin, eep->calTargetFbinCck, AR9380_NUM_2G_CCK_TARGET_POWERS, &lo, &hi);
    for (uint32_t i = 0; i < 4; i++)
        cck[i] = target_power(fbin, eep->calTargetFbinCck, lo, hi,
                              &eep->calTargetPowerCck[lo].tPow2x[i], &eep->calTargetPowerCck[hi].tPow2x[i]);
    target_pick(fbin, eep->calTargetFbin2G, AR9380_NUM_2G_20_TARGET_POWERS, &lo, &hi);
    for (uint32_t i = 0; i < 4; i++)
        ofdm[i] = target_power(fbin, eep->calTargetFbin2G, lo, hi,
                               &eep->calTargetPower2G[lo].tPow2x[i], &eep->calTargetPower2G[hi].tPow2x[i]);
    target_pick(fbin, eep->calTargetFbin2GHT20, AR9380_NUM_2G_20_TARGET_POWERS, &lo, &hi);
    for (uint32_t i = 0; i < 14; i++)
        ht[i] = target_power(fbin, eep->calTargetFbin2GHT20, lo, hi,
                             &eep->calTargetPower2GHT20[lo].tPow2x[i], &eep->calTargetPower2GHT20[hi].tPow2x[i]);
    c->txpower[0] = cck[0];
    c->txpower[1] = ofdm[0];
    c->txpower[2] = ofdm[1];
    c->txpower[3] = ofdm[3];

    // ar9003_write_txpower. Every frame's descriptor says "the rate's own
    // power" (AR_MAX_RATE_POWER), so these are what goes on the air.
    wr(c, AR_PHY_TX_FORCED_GAIN, 0);
    // OFDM 6, 9, 12, 18 and 24, 36, 48, 54
    wr(c, AR_PHY_PWRTX_RATE1, POW(ofdm[0], 24) | POW(ofdm[0], 16) | POW(ofdm[0], 8) | POW(ofdm[0], 0));
    wr(c, AR_PHY_PWRTX_RATE2, POW(ofdm[3], 24) | POW(ofdm[2], 16) | POW(ofdm[1], 8) | POW(ofdm[0], 0));
    // CCK 1 long, 2 long and short; 5.5 and 11, long and short
    wr(c, AR_PHY_PWRTX_RATE3, POW(cck[0], 24) | POW(cck[0], 16) | POW(cck[0], 0));
    wr(c, AR_PHY_PWRTX_RATE4, POW(cck[3], 24) | POW(cck[2], 16) | POW(cck[1], 8) | POW(cck[0], 0));
    // HT20 (never sent, but PA predistortion reads these), HT40 off
    wr(c, AR_PHY_PWRTX_RATE5, POW(ht[3], 24) | POW(ht[2], 16) | POW(ht[1], 8) | POW(ht[0], 0));
    wr(c, AR_PHY_PWRTX_RATE6, POW(ht[7], 24) | POW(ht[6], 16) | POW(ht[5], 8) | POW(ht[4], 0));
    wr(c, AR_PHY_PWRTX_RATE7, 0);
    wr(c, AR_PHY_PWRTX_RATE8, 0);
    wr(c, AR_PHY_PWRTX_RATE10, POW(ht[11], 24) | POW(ht[10], 16) | POW(ht[9], 8) | POW(ht[8], 0));
    wr(c, AR_PHY_PWRTX_RATE11, POW(ht[13], 8) | POW(ht[12], 0));
    wr(c, AR_PHY_PWRTX_RATE12, 0);

    // Open loop power control: the chain's correction and temperature at
    // calibration, from the ROM's calibration piers
    const uint8_t* piers = eep->calFreqPier2G;
    const struct ar9380_cal_data_per_freq_op_loop* data = eep->calPierData2G[0];
    uint32_t n = AR9380_NUM_2G_CAL_PIERS;
    uint32_t i = 0;
    while (i < n && piers[i] != AR_BCHAN_UNUSED && piers[i] <= fbin)
        i++;
    hi = i;
    if (i == 0) {
        lo = hi;
    } else {
        lo = i - 1;
        if (hi == n || piers[hi] == AR_BCHAN_UNUSED)
            hi = lo;
    }
    if (hi >= n)                // Every pier unused: the first one's values
        lo = hi = 0;
    int32_t corr = interpolate(fbin, piers[lo], data[lo].refPower, piers[hi], data[hi].refPower);
    int32_t temp = interpolate(fbin, piers[lo], data[lo].tempMeas, piers[hi], data[hi].tempMeas);

    uint32_t reg = rd(c, AR_PHY_TPC_11_B(0));
    wr(c, AR_PHY_TPC_11_B(0), RW(reg, AR_PHY_TPC_11_OLPC_GAIN_DELTA, (uint32_t)corr));
    reg = rd(c, AR_PHY_TPC_6_B(0));
    wr(c, AR_PHY_TPC_6_B(0), RW(reg, AR_PHY_TPC_6_ERROR_EST_MODE, 3));
    reg = rd(c, AR_PHY_TPC_19);
    wr(c, AR_PHY_TPC_19, RW(reg, AR_PHY_TPC_19_ALPHA_THERM, (uint32_t)(int32_t)eep->modalHeader2G.tempSlope));
    reg = rd(c, AR_PHY_TPC_18);
    wr(c, AR_PHY_TPC_18, RW(reg, AR_PHY_TPC_18_THERM_CAL, (uint32_t)temp));
}

#undef POW

static void ar9485_set_synth(struct Ar9485* c, uint32_t freq) {
    // 2.4 GHz on the AR9485: ((freq << 16) - 215) / 15
    uint32_t chansel = ((freq << 16) - 215) / 15;
    wr(c, AR_PHY_SYNTH_CONTROL, AR9380_BMODE);
    setbits(c, AR_PHY_65NM_CH0_SYNTH4, AR_PHY_SYNTH4_LONG_SHIFT_SELECT);
    uint32_t phy = (chansel << 2) | AR9380_FRACMODE;
    wr(c, AR_PHY_65NM_CH0_SYNTH7, phy);
    // Toggle "load synth channel"
    wr(c, AR_PHY_65NM_CH0_SYNTH7, phy | AR9380_LOAD_SYNTH);
}

// AGC offset calibration and the first noise floor measurement
static bool ar9485_init_calib(struct Ar9485* c) {
    ar9485_init_chains(c);
    // Disable and re-enable the PHY chips
    wr(c, AR_PHY_ACTIVE, AR_PHY_ACTIVE_DIS);
    delay(c, 5);
    wr(c, AR_PHY_ACTIVE, AR_PHY_ACTIVE_EN);

    setbits(c, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_CAL);
    if (!wait_for(c, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_CAL, 0, 10000, 10))
        return fail(c, "AGC calibration did not finish");
    ar9485_init_chains(c);

    setbits(c, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_NF);
    return true;
}

static void ar9485_interrupts_off(struct Ar9485* c) {
    // This driver polls: no line, no MSI
    wr(c, AR_IER, 0);
    wr(c, AR_IMR, 0);
    wr(c, AR_ISR, 0xFFFFFFFFU);
    wr(c, AR_INTR_SYNC_CAUSE, 0xFFFFFFFFU);
    wr(c, AR_INTR_SYNC_ENABLE, 0);
    wr(c, AR_INTR_SYNC_MASK, 0);
    wr(c, AR_INTR_ASYNC_ENABLE, 0);
    wr(c, AR_INTR_ASYNC_MASK, 0);
    wr(c, AR_INTR_PRIO_ASYNC_ENABLE, 0);
    wr(c, AR_INTR_PRIO_ASYNC_MASK, 0);
    wr(c, AR_INTR_PRIO_SYNC_ENABLE, 0);
    wr(c, AR_INTR_PRIO_SYNC_MASK, 0);
}

static void ar9485_init_dma(struct Ar9485* c) {
    uint32_t reg = rd(c, AR_TXCFG);
    wr(c, AR_TXCFG, RW(reg, AR_TXCFG_DMASZ, AR_DMASZ_128B));
    reg = rd(c, AR_RXCFG);
    wr(c, AR_RXCFG, RW(reg, AR_RXCFG_DMASZ, AR_DMASZ_128B));
    wr(c, AR_RXFIFO_CFG, 512);
    wr(c, AR_PCU_TXBUF_CTRL, AR_PCU_TXBUF_CTRL_USABLE_SIZE);

    // Transmit status ring (ar9003_reset_txsring): the card writes one
    // status per frame sent, in order, wrapping at the end
    c->tx_tail = 0;
    c->tx_queued = 0;
    c->status_next = 0;
    if (c->tx_on) {
        memset(c->tx_status.virt, 0, AR9485_TX_STATUS_COUNT * AR9485_TX_STATUS_SIZE);
        wr(c, AR_Q_STATUS_RING_START, c->tx_status.phys);
        wr(c, AR_Q_STATUS_RING_END, c->tx_status.phys + AR9485_TX_STATUS_COUNT * AR9485_TX_STATUS_SIZE);
    }
}

//
// Transmit set-up (athn_hw_reset: the queues, QoS, interframe timing).
// 2.4 GHz with long slots, which every access point takes.
//

#define SIFS_US         10
#define SLOT_US         20
#define ACK_TIMEOUT_US  64      // athn: sifs + slot, raised to 64 on 2.4 GHz (early timeouts)
#define CTS_TIMEOUT_US  (ACK_TIMEOUT_US + 48 - SIFS_US - SLOT_US)

static void ar9485_init_tx(struct Ar9485* c) {
    for (uint32_t i = 0; i < AR_NUM_DCU; i++)
        wr(c, AR_DQCUMASK(i), 1U << i);

    // athn_init_tx_queues, for the one queue used, and ath9k's default
    // contention window (15-1023, AIFS 2)
    uint32_t q = AR9485_TX_QUEUE;
    wr(c, AR_DRETRY_LIMIT(q), SM(AR_D_RETRY_LIMIT_STA_SH, 32) | SM(AR_D_RETRY_LIMIT_STA_LG, 32) |
                              SM(AR_D_RETRY_LIMIT_FR_SH, 10));
    wr(c, AR_QMISC(q), AR_Q_MISC_DCU_EARLY_TERM_REQ);
    wr(c, AR_DMISC(q), SM(AR_D_MISC_BKOFF_THRESH, 2) | AR_D_MISC_CW_BKOFF_EN | AR_D_MISC_FRAG_WAIT_EN);
    wr(c, AR_DLCL_IFS(q), SM(AR_D_LCL_IFS_CWMIN, 15) | SM(AR_D_LCL_IFS_CWMAX, 1023) | SM(AR_D_LCL_IFS_AIFS, 2));
    // The card checks each descriptor's checksum before sending
    wr(c, AR_Q_DESC_CRCCHK, AR_Q_DESC_CRCCHK_EN);

    // athn_init_qos (no-ack policy bits: 2 bits at bit 5 of byte 0)
    wr(c, AR_MIC_QOS_CONTROL, 0x100aa);
    wr(c, AR_MIC_QOS_SELECT, 0x3210);
    wr(c, AR_QOS_NO_ACK, 0x52);
    wr(c, AR_TXOP_X, AR_TXOP_X_VAL);
    wr(c, AR_TXOP_0_3, 0xFFFFFFFFU);
    wr(c, AR_TXOP_4_7, 0xFFFFFFFFU);
    wr(c, AR_TXOP_8_11, 0xFFFFFFFFU);
    wr(c, AR_TXOP_12_15, 0xFFFFFFFFU);
    setbits(c, AR_PCU_MISC, AR_PCU_MIC_NEW_LOC_ENA);

    // athn_setsifs, athn_updateslot, ack and CTS timeouts (MAC clocks)
    wr(c, AR_D_GBL_IFS_SIFS, (SIFS_US - 2) * AR_CLOCK_RATE_2GHZ_OFDM);
    wr(c, AR_D_GBL_IFS_SLOT, SLOT_US * AR_CLOCK_RATE_2GHZ_OFDM);
    uint32_t reg = rd(c, AR_TIME_OUT);
    reg = RW(reg, AR_TIME_OUT_ACK, ACK_TIMEOUT_US * AR_CLOCK_RATE_2GHZ_OFDM);
    reg = RW(reg, AR_TIME_OUT_CTS, CTS_TIMEOUT_US * AR_CLOCK_RATE_2GHZ_OFDM);
    wr(c, AR_TIME_OUT, reg);
}

// Stop the transmit queue (ath9k_hw_stoptxdma): the card stops reading
// our buffers. Bounded: a dead card gives up after a few milliseconds.
static void ar9485_tx_stop(struct Ar9485* c) {
    if (!c->tx_on)
        return;
    uint32_t q = AR9485_TX_QUEUE;
    wr(c, AR_Q_TXD, 1U << q);
    for (uint32_t i = 0; i < 100; i++) {
        if (!(rd(c, AR_Q_TXE) & (1U << q)) && MS(rd(c, AR_QSTS(q)), AR_Q_STS_PEND_FR_CNT) == 0)
            break;
        delay(c, 100);
    }
    wr(c, AR_Q_TXD, 0);
    // Whatever was queued is gone
    c->tx_tail = 0;
    c->tx_queued = 0;
}

static void ar9485_set_opmode_sta(struct Ar9485* c) {
    uint32_t reg = rd(c, AR_STA_ID1);
    reg &= ~(AR_STA_ID1_ADHOC | AR_STA_ID1_STA_AP);
    reg |= AR_STA_ID1_KSRCH_MODE;
    wr(c, AR_STA_ID1, reg);
}

//
// Receiving (ar9003.c: EDMA). The card takes buffer addresses through two
// FIFOs (AR_LP_RXDP, AR_HP_RXDP) and fills them in that order: status
// words first, then the frame. It sets DONE in the status when it's done.
//

static struct Ar9485Page* rx_buf(struct Ar9485* c, bool hp, uint32_t i) {
    return &c->rx[hp ? AR9485_RX_LP_COUNT + i : i];
}

static void rx_give(struct Ar9485* c, bool hp, struct Ar9485Page* b) {
    memset(b->virt, 0, AR9485_RX_STATUS_SIZE);
    wr(c, hp ? AR_HP_RXDP : AR_LP_RXDP, b->phys);
}

static void ar9485_rx_start(struct Ar9485* c) {
    uint32_t reg = rd(c, AR_RXBP_THRESH);
    reg = RW(reg, AR_RXBP_THRESH_HP, 1);
    reg = RW(reg, AR_RXBP_THRESH_LP, 1);
    wr(c, AR_RXBP_THRESH, reg);
    wr(c, AR_DATABUF_SIZE, AR9485_RX_DATA_MAX);

    for (uint32_t i = 0; i < AR9485_RX_LP_COUNT; i++)
        rx_give(c, false, rx_buf(c, false, i));
    for (uint32_t i = 0; i < AR9485_RX_HP_COUNT; i++)
        rx_give(c, true, rx_buf(c, true, i));
    c->lp_next = 0;
    c->hp_next = 0;
    wr(c, AR_CR, 0);

    // Beacons from any network, and nothing addressed to us until
    // ar9485_set_bss
    wr(c, AR_RX_FILTER, AR_RX_FILTER_BCAST | AR_RX_FILTER_BEACON);
    wr(c, AR_PHY_ERR, 0);
    clrbits(c, AR_RXCFG, AR_RXCFG_ZLFDMA);

    wr(c, AR_BSSMSKL, 0xFFFFFFFFU);
    wr(c, AR_BSSMSKU, 0xFFFF);
    ar9485_set_opmode_sta(c);
    wr(c, AR_MCAST_FIL0, 0xFFFFFFFFU);
    wr(c, AR_MCAST_FIL1, 0xFFFFFFFFU);
    wr(c, AR_FILT_OFDM, 0);
    wr(c, AR_FILT_CCK, 0);
    wr(c, AR_MIBC, 0);
    wr(c, AR_PHY_ERR_MASK_1, AR_PHY_ERR_OFDM_TIMING);
    wr(c, AR_PHY_ERR_MASK_2, AR_PHY_ERR_CCK_TIMING);
    wr(c, AR_PHY_ERR_1, 0);
    wr(c, AR_PHY_ERR_2, 0);

    // No keys: hardware crypto off
    setbits(c, AR_DIAG_SW, AR_DIAG_ENCRYPT_DIS | AR_DIAG_DECRYPT_DIS);
    // Start the PCU receiving
    clrbits(c, AR_DIAG_SW, AR_DIAG_RX_DIS | AR_DIAG_RX_ABORT);
    c->rx_on = true;
}

// Stop the receive DMA. After this the card no longer writes to the buffers.
static bool ar9485_rx_stop(struct Ar9485* c) {
    setbits(c, AR_DIAG_SW, AR_DIAG_RX_DIS | AR_DIAG_RX_ABORT);
    wr(c, AR_RX_FILTER, 0);
    wr(c, AR_CR, AR_CR_RXD);
    bool stopped = wait_for(c, AR_CR, AR_CR_RXE, 0, 100, 100);
    c->rx_on = false;
    return stopped;
}

// One buffer the card filled: its status (already copied) checked, and
// the frame copied out of DMA memory into c->frame, so the card can't
// rewrite it under the parser. False for a buffer to drop.
static bool rx_take(struct Ar9485* c, const uint32_t* st, const uint8_t* src, struct WlanRxFrame* f) {
    // A real receive status: Atheros's id, not a transmit status
    if ((st[0] & (AR_RXI_DESC_ID_M | AR_RXI_DESC_TX | AR_RXI_CTRL_STAT)) != SM(AR_RXI_DESC_ID, AR_VENDOR_ATHEROS)) {
        c->rx_bad++;
        return false;
    }
    if (!(st[11] & AR_RXS11_FRAME_OK)) {
        c->rx_errors++;             // CRC, PHY or decrypt error
        return false;
    }
    uint32_t len = MS(st[2], AR_RXS2_DATA_LEN);
    // A frame split over buffers (bigger than any beacon), or a length the
    // buffer can't hold: dropped
    if ((st[2] & AR_RXS2_MORE) || len < AR9485_RX_MIN_LEN || len > AR9485_RX_DATA_MAX) {
        c->rx_bad++;
        return false;
    }
    memcpy(c->frame, src, len);

    // The card puts 2 bytes after a header that isn't a multiple of 4 long
    // (QoS data: 26), as ath9k's receive path knows. Taken out again.
    uint8_t fc0 = c->frame[0], fc1 = c->frame[1];
    if ((fc0 & 0x0c) == 0x08 && (fc0 & 0x80)) {
        uint32_t hdr = 26 + ((fc1 & 0x03) == 0x03 ? 6 : 0) + ((fc1 & 0x80) ? 4 : 0);
        if (hdr % 4 != 0) {
            uint32_t pad = 4 - hdr % 4;
            if (len < hdr + pad + 4) {
                c->rx_bad++;
                return false;
            }
            memmove(c->frame + hdr, c->frame + hdr + pad, len - hdr - pad);
            len -= pad;
        }
    }

    int8_t rssi = (int8_t)MS(st[5], AR_RXS5_RSSI_COMBINED);
    uint8_t rate = (uint8_t)MS(st[1], AR_RXS1_RATE);
    f->data   = c->frame;
    f->length = len - 4;                        // Without the FCS
    f->rssi   = WLAN_RSSI_UNKNOWN;
    if (rssi != AR9485_RSSI_BAD && rssi >= -20 && rssi <= 95)
        f->rssi = (int8_t)(rssi + AR9485_NOISE_FLOOR);
    f->cck    = !(rate & 0x80) && (rate & 0x10);
    c->rx_frames++;
    return true;
}

uint32_t ar9485_rx_harvest(struct Ar9485* c, WlanRxFn fn, void* ctx) {
    if (!c->rx_on)
        return 0;
    uint32_t handed = 0;
    for (int q = 0; q < 2; q++) {
        bool hp = q == 1;
        uint32_t count = hp ? AR9485_RX_HP_COUNT : AR9485_RX_LP_COUNT;
        uint8_t* next = hp ? &c->hp_next : &c->lp_next;
        // At most one pass over the queue: the card can't keep us here
        for (uint32_t n = 0; n < count; n++) {
            struct Ar9485Page* b = rx_buf(c, hp, *next);
            const volatile uint32_t* vst = (const volatile uint32_t*)b->virt;
            if (!(vst[11] & AR_RXS11_DONE))
                break;
            uint32_t st[AR9485_RX_STATUS_SIZE / 4];
            for (uint32_t i = 0; i < AR9485_RX_STATUS_SIZE / 4; i++)
                st[i] = vst[i];

            struct WlanRxFrame f;
            bool ok = rx_take(c, st, b->virt + AR9485_RX_STATUS_SIZE, &f);
            // Back to the card (at the tail), then parse our copy
            rx_give(c, hp, b);
            *next = (uint8_t)((*next + 1) % count);
            if (ok) {
                fn(ctx, &f);
                handed++;
            }
        }
    }
    // Receiving stops when a FIFO runs dry: start it again
    wr(c, AR_CR, 0);
    return handed;
}

//
// Transmitting (ar9003.c: ar9003_tx, ar9003_tx_process). Each buffer is a
// page: the descriptor, then the frame. Writing the descriptor's address
// to AR_QTXDP queues it in the queue's FIFO (8 deep, as many as there are
// buffers); the card sends it, retries until acknowledged or out of tries,
// and writes a status into the ring. One queue, so statuses come back in
// the order frames went in.
//

static bool tx_pages_ok(const struct Ar9485* c) {
    if (c->tx_status.virt == NULL || c->tx_status.phys == 0)
        return false;
    for (uint32_t i = 0; i < AR9485_TX_COUNT; i++) {
        if (c->tx[i].virt == NULL || c->tx[i].phys == 0)
            return false;
    }
    return true;
}

static bool rate_is_cck(uint8_t rate) {
    return rate >= AR9485_RATE_CCK11 && rate <= AR9485_RATE_CCK1;
}

// How long the acknowledgement takes, in microseconds, which the card
// writes into the frame's duration field (athn_txtime of a 14-byte ACK at
// the control response rate; long preamble)
static uint32_t ack_time_us(uint8_t rate) {
    if (rate_is_cck(rate))
        return rate == AR9485_RATE_CCK1 ? 314 : 258;    // ACK at 1 or 2 Mbit/s, + preamble + SIFS
    switch (rate) {
        case AR9485_RATE_OFDM6:
        case AR9485_RATE_OFDM9:  return 60;             // ACK at 6 Mbit/s
        case AR9485_RATE_OFDM12:
        case AR9485_RATE_OFDM18: return 48;             // 12
        default:                 return 44;             // 24
    }
}

static bool rate_known(uint8_t rate) {
    switch (rate) {
        case AR9485_RATE_CCK1:   case AR9485_RATE_CCK2:   case AR9485_RATE_CCK5:   case AR9485_RATE_CCK11:
        case AR9485_RATE_OFDM6:  case AR9485_RATE_OFDM9:  case AR9485_RATE_OFDM12: case AR9485_RATE_OFDM18:
        case AR9485_RATE_OFDM24: case AR9485_RATE_OFDM36: case AR9485_RATE_OFDM48: case AR9485_RATE_OFDM54:
            return true;
    }
    return false;
}

bool ar9485_tx(struct Ar9485* c, const uint8_t* frame, uint32_t length, const struct Ar9485TxRates* rates) {
    if (!c->tx_on)
        return fail(c, "not tuned to transmit");
    if (frame == NULL || length < 10 || length > AR9485_TX_DATA_MAX)
        return fail(c, "bad frame length");
    if (c->tx_queued >= AR9485_TX_COUNT)
        return fail(c, "every transmit buffer is in the card");

    uint8_t rate[4], tries[4];
    for (uint32_t i = 0; i < 4; i++) {
        rate[i]  = rates->rate[i];
        tries[i] = rates->tries[i] > 15 ? 15 : rates->tries[i];
        if (tries[i] != 0 && !rate_known(rate[i]))
            return fail(c, "bad rate");
    }
    if (tries[0] == 0)
        return fail(c, "no rate");
    // To a group: once, at the first rate, nobody acknowledges it
    bool group = (frame[4] & 0x01) != 0;
    if (group) {
        tries[0] = 1;
        tries[1] = tries[2] = tries[3] = 0;
    }
    for (uint32_t i = 1; i < 4; i++) {
        if (tries[i] == 0)
            rate[i] = rate[0];
    }

    struct Ar9485Page* b = &c->tx[(c->tx_tail + c->tx_queued) % AR9485_TX_COUNT];
    memcpy(b->virt + AR9485_TX_DESC_SIZE, frame, length);

    uint32_t d[AR9485_TX_DESC_SIZE / 4];
    memset(d, 0, sizeof(d));
    d[0]  = SM(AR_TXI_DESC_ID, AR_VENDOR_ATHEROS) | SM(AR_TXI_DESC_NDWORDS, 23) |
            SM(AR_TXI_QCU_NUM, AR9485_TX_QUEUE) | AR_TXI_DESC_TX | AR_TXI_CTRL_STAT;
    d[1]  = 0;                                      // Link: one buffer per frame
    d[2]  = b->phys + AR9485_TX_DESC_SIZE;
    d[3]  = SM(AR_TXC_BUF_LEN, length);
    // The card adds the FCS: the frame on the air is 4 bytes longer.
    // Power: the rate's own (the per-rate registers).
    d[11] = SM(AR_TXC11_FRAME_LEN, length + 4) | SM(AR_TXC11_XMIT_POWER, AR_MAX_RATE_POWER) |
            AR_TXC11_CLR_DEST_MASK;
    d[12] = SM(AR_TXC12_FRAME_TYPE, AR_FRAME_TYPE_NORMAL) | (group ? AR_TXC12_NO_ACK : 0);
    d[13] = SM(AR_TXC13_XMIT_DATA_TRIES0, tries[0]) | SM(AR_TXC13_XMIT_DATA_TRIES1, tries[1]) |
            SM(AR_TXC13_XMIT_DATA_TRIES2, tries[2]) | SM(AR_TXC13_XMIT_DATA_TRIES3, tries[3]) |
            AR_TXC13_DUR_UPDATE_ENA;
    d[14] = SM(AR_TXC14_XMIT_RATE0, rate[0]) | SM(AR_TXC14_XMIT_RATE1, rate[1]) |
            SM(AR_TXC14_XMIT_RATE2, rate[2]) | SM(AR_TXC14_XMIT_RATE3, rate[3]);
    if (!group) {
        d[15] = SM(AR_TXC15_PACKET_DUR0, ack_time_us(rate[0])) | SM(AR_TXC15_PACKET_DUR1, ack_time_us(rate[1]));
        d[16] = SM(AR_TXC16_PACKET_DUR2, ack_time_us(rate[2])) | SM(AR_TXC16_PACKET_DUR3, ack_time_us(rate[3]));
    }
    d[17] = SM(AR_TXC17_ENCR_TYPE, AR_ENCR_TYPE_CLEAR);      // Encrypted already, if at all
    d[18] = SM(AR_TXC18_CHAIN_SEL0, c->txmask) | SM(AR_TXC18_CHAIN_SEL1, c->txmask) |
            SM(AR_TXC18_CHAIN_SEL2, c->txmask) | SM(AR_TXC18_CHAIN_SEL3, c->txmask);
    d[19] = AR_TXC19_NOT_SOUNDING;
    // Checksum of the first ten words (the card checks it: AR_Q_DESC_CRCCHK)
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 10; i++)
        sum += d[i];
    sum = (sum >> 16) + (sum & 0xffff);
    d[10] = SM(AR_TXC10_PTR_CHK_SUM, sum);
    memcpy(b->virt, d, sizeof(d));

    // The descriptor and frame are in memory before the card hears of them
    // (x86 keeps stores in order; this keeps the compiler from moving them)
    __asm__ volatile("" ::: "memory");
    wr(c, AR_QTXDP(AR9485_TX_QUEUE), b->phys);
    c->tx_queued++;
    c->tx_frames++;
    return true;
}

uint32_t ar9485_tx_reclaim(struct Ar9485* c) {
    if (!c->tx_on)
        return 0;
    uint32_t done = 0;
    // At most once round the ring: the card can't keep us here
    for (uint32_t n = 0; n < AR9485_TX_STATUS_COUNT; n++) {
        volatile uint32_t* vst = (volatile uint32_t*)(c->tx_status.virt + c->status_next * AR9485_TX_STATUS_SIZE);
        if (!(vst[8] & AR_TXS8_DONE))
            break;
        uint32_t st[AR9485_TX_STATUS_SIZE / 4];
        for (uint32_t i = 0; i < AR9485_TX_STATUS_SIZE / 4; i++) {
            st[i] = vst[i];
            vst[i] = 0;
        }
        c->status_next = (uint8_t)((c->status_next + 1) % AR9485_TX_STATUS_COUNT);

        // A transmit status from our queue, for a frame we did queue
        bool valid = (st[0] & (AR_TXI_DESC_ID_M | AR_TXI_DESC_TX)) ==
                     (SM(AR_TXI_DESC_ID, AR_VENDOR_ATHEROS) | AR_TXI_DESC_TX) &&
                     MS(st[0], AR_TXI_QCU_NUM) == AR9485_TX_QUEUE;
        if (!valid || c->tx_queued == 0) {
            c->tx_bad++;
            continue;
        }
        c->tx_tail = (uint8_t)((c->tx_tail + 1) % AR9485_TX_COUNT);
        c->tx_queued--;
        done++;
        if (st[3] & AR_TXS3_EXCESSIVE_RETRIES)
            c->tx_failed++;             // Never acknowledged
        else
            c->tx_acked++;              // Or a group frame, sent
    }
    return done;
}

//
// Channels
//

bool ar9485_tune(struct Ar9485* c, uint8_t channel) {
    if (channel < 1 || channel > WLAN_CHANNEL_MAX)
        return fail(c, "bad channel");
    uint32_t freq = 2407U + 5U * channel;
    c->switched_off = false;

    ar9485_tx_stop(c);
    c->tx_on = false;
    c->bss_set = false;
    if (c->rx_on)
        ar9485_rx_stop(c);
    if (!ar9485_set_power_awake(c))
        return false;

    // Kept over the reset
    uint32_t def_ant = rd(c, AR_DEF_ANTENNA);
    if (def_ant == 0)
        def_ant = 1;
    uint32_t sta_id1 = rd(c, AR_STA_ID1) & AR_STA_ID1_BASE_RATE_11B;
    uint32_t cfg_led = rd(c, AR_CFG_LED) & (AR_CFG_LED_ASSOC_CTL_M | AR_CFG_LED_MODE_SEL_M |
                                            AR_CFG_LED_BLINK_THRESH_SEL_M | AR_CFG_LED_BLINK_SLOW);

    wr(c, AR_PHY_ACTIVE, AR_PHY_ACTIVE_DIS);
    if (!ar9485_reset(c, false))
        return false;
    if (!ar9485_set_power_awake(c))
        return false;

    ar9485_init_pll(c);
    wr(c, AR_PHY_MODE, AR_PHY_MODE_DYNAMIC);

    if (ar9485_radio_switched_off(c)) {
        c->switched_off = true;
        return fail(c, "the wireless switch has the radio off");
    }
    setbits(c, AR_GPIO_INPUT_EN_VAL, AR_GPIO_JTAG_DISABLE);

    ar9485_hw_init(c);
    ar9485_set_txpower(c, freq);
    ar9485_set_delta_slope(c, freq);
    ar9485_init_from_rom(c);

    // Our address: frames to it are taken and acknowledged
    const uint8_t* m = c->mac;
    wr(c, AR_STA_ID0, (uint32_t)m[0] | (uint32_t)m[1] << 8 | (uint32_t)m[2] << 16 | (uint32_t)m[3] << 24);
    wr(c, AR_STA_ID1, ((uint32_t)m[4] | (uint32_t)m[5] << 8) | sta_id1 |
                      AR_STA_ID1_RTS_USE_DEF | AR_STA_ID1_CRPT_MIC_ENABLE);
    ar9485_set_opmode_sta(c);
    wr(c, AR_BSSMSKL, 0xFFFFFFFFU);
    wr(c, AR_BSSMSKU, 0xFFFF);
    wr(c, AR_DEF_ANTENNA, def_ant);
    wr(c, AR_BSS_ID0, 0);
    wr(c, AR_BSS_ID1, 0);
    wr(c, AR_RSSI_THR, SM(AR_RSSI_THR_BM_THR, 7));

    ar9485_set_synth(c, freq);
    ar9485_interrupts_off(c);
    ar9485_init_tx(c);

    // MAC clock: 44 MHz for 2.4 GHz OFDM
    uint32_t reg = rd(c, AR_USEC);
    wr(c, AR_USEC, RW(reg, AR_USEC_USEC, AR_CLOCK_RATE_2GHZ_OFDM - 1));
    // The sequence numbers are the station's
    setbits(c, AR_STA_ID1, AR_STA_ID1_PRESERVE_SEQNUM);
    c->tx_on = tx_pages_ok(c);
    ar9485_init_dma(c);

    // Activate the PHY (baseband and synthesizer)
    uint32_t synth_delay = ar9485_synth_delay(c);
    wr(c, AR_PHY_ACTIVE, AR_PHY_ACTIVE_EN);
    delay(c, AR_BASE_PHY_ACTIVE_DELAY + synth_delay);

    if (!ar9485_init_calib(c)) {
        c->tx_on = false;
        return false;
    }
    wr(c, AR_CFG_LED, cfg_led | AR_CFG_SCLK_32KHZ);

    ar9485_rx_start(c);

    // A card that dropped off the bus part way through reads as all ones,
    // and every write since went nowhere
    if (rd(c, AR_SREV) == 0xFFFFFFFFU) {
        ar9485_tx_stop(c);
        c->tx_on = false;
        ar9485_rx_stop(c);
        return fail(c, "the card stopped answering");
    }
    c->channel = channel;
    return true;
}

void ar9485_set_bss(struct Ar9485* c, const uint8_t bssid[6]) {
    memcpy(c->bssid, bssid, 6);
    c->bss_set = true;
    wr(c, AR_BSS_ID0, (uint32_t)bssid[0] | (uint32_t)bssid[1] << 8 | (uint32_t)bssid[2] << 16 |
                      (uint32_t)bssid[3] << 24);
    wr(c, AR_BSS_ID1, (uint32_t)bssid[4] | (uint32_t)bssid[5] << 8);
    // athn_rx_start's station filter: to us, group frames of this BSS, and
    // every beacon (ours tell the station the access point is still there)
    if (c->rx_on)
        wr(c, AR_RX_FILTER, AR_RX_FILTER_UCAST | AR_RX_FILTER_MCAST | AR_RX_FILTER_BCAST | AR_RX_FILTER_BEACON);
}

void ar9485_stop(struct Ar9485* c) {
    // Let what's queued go out first (a goodbye to the access point, say),
    // for at most 20 ms
    for (uint32_t i = 0; c->tx_on && i < 200; i++) {
        ar9485_tx_reclaim(c);
        if (c->tx_queued == 0)
            break;
        delay(c, 100);
    }
    ar9485_tx_stop(c);
    c->tx_on = false;
    c->bss_set = false;
    if (c->rx_on)
        ar9485_rx_stop(c);
    c->rx_on = false;
    wr(c, AR_PHY_ACTIVE, AR_PHY_ACTIVE_DIS);
    ar9485_set_power_sleep(c);
}
