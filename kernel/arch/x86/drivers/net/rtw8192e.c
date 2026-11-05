#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/arch/x86/drivers/net/rtw8192e.h>
#include <kernel/arch/x86/drivers/net/rtw8192e_tables.h>
#include <kernel/util/string.h>

//
// RTL8192EU bring-up for receiving. The sequence and values follow
// FreeBSD's rtwn(4) driver (rtwn_init() with the rtl8192e / r92eu parts,
// BSD licensed, see rtw8192e_tables.h), trimmed to what a passive scan
// needs: no transmit power, encryption, beacons or rate control.
//
// Any failed register access sets r->failed and every later access is
// skipped, so a stick pulled out half way through ends the sequence in a
// few calls instead of hundreds of timeouts.
//

// Registers (names as in FreeBSD's r92c_reg.h / r92e_reg.h)
#define REG_SYS_FUNC_EN         0x0002
#define   FEN_BBRSTB            0x0001
#define   FEN_BB_GLB_RST        0x0002
#define   FEN_USBA              0x0004
#define   FEN_USBD              0x0010
#define   FEN_CPUEN             0x0400
#define   FEN_DIO_RF            0x2000
#define REG_APS_FSMCO           0x004
#define   APS_APFM_ONMAC        0x00000100
#define   APS_APFM_RSM          0x00000400
#define   APS_AFSM_HSUS         0x00000800
#define   APS_AFSM_PCIE         0x00001000
#define   APS_APDM_HPDN         0x00008000
#define   APS_RDY_MACON         0x00010000
#define   APS_SUS_HOST          0x00020000
#define REG_LDOV12_CTRL         0x014
#define REG_RSV_CTRL            0x01C
#define REG_RF_CTRL             0x01F
#define   RF_CTRL_EN            0x01
#define   RF_CTRL_RSTB          0x02
#define   RF_CTRL_SDMRSTB       0x04
#define REG_AFE_XTAL_CTRL       0x024
#define REG_AFE_PLL_CTRL        0x028
#define   AFE_PLL_FREF_SEL      0x04
#define REG_R92E_XTAL_CTRL      0x02C
#define   R92E_XTAL_ADDR_M      0x00FFF000
#define   R92E_XTAL_ADDR_S      12
#define REG_ACLK_MON            0x03E
#define REG_GPIO_MUXCFG         0x040
#define   GPIO_MUXCFG_ENBT      0x20
#define REG_APE_PLL_CTRL_EXT    0x078
#define REG_LDO_SWR_CTRL        0x07C
#define REG_MCUFWDL             0x080
#define   MCUFWDL_EN            0x00000001
#define   MCUFWDL_RDY           0x00000002
#define   MCUFWDL_CHKSUM_RPT    0x00000004
#define   MCUFWDL_WINTINI_RDY   0x00000040
#define   MCUFWDL_RAM_DL_SEL    0x00000080
#define   MCUFWDL_PAGE_M        0x00070000
#define   MCUFWDL_PAGE_S        16
#define   MCUFWDL_ROM_DLEN      0x00080000
#define REG_HIMR                0x0B0
#define REG_HIMRE               0x0B8
#define REG_SYS_CFG             0x00F0
#define   SYS_CFG_TRP_BT_EN     0x01000000
#define REG_CR                  0x100
#define   CR_HCI_TXDMA_EN       0x0001
#define   CR_HCI_RXDMA_EN       0x0002
#define   CR_TXDMA_EN           0x0004
#define   CR_RXDMA_EN           0x0008
#define   CR_PROTOCOL_EN        0x0010
#define   CR_SCHEDULE_EN        0x0020
#define   CR_MACTXEN            0x0040
#define   CR_MACRXEN            0x0080
#define   CR_CALTMR_EN          0x0400
#define REG_MSR                 0x102
#define REG_TRXDMA_CTRL         0x10C
#define   TRXDMA_QMAP_M         0xFFF0
#define   TRXDMA_QMAP_3EP       0xF5B0
#define REG_TRXFF_BNDY          0x114
#define REG_RQPN                0x200
#define   RQPN_LD               0x80000000u
#define REG_TDECTRL             0x208
#define REG_TXDMA_OFFSET_CHK    0x20C
#define   TXDMA_DROP_DATA_EN    0x00000200
#define REG_RQPN_NPQ            0x214
#define REG_AUTO_LLT            0x224
#define   AUTO_LLT_INIT         0x00010000
#define REG_RXDMA_PRO           0x290
#define REG_FWHW_TXQ_CTRL       0x420
#define   FWHW_AMPDU_RTY_NEW    0x80
#define REG_HWSEQ_CTRL          0x423
#define REG_TXPKTBUF_BCNQ_BDNY  0x424
#define REG_TXPKTBUF_MGQ_BDNY   0x425
#define REG_RL                  0x42A
#define REG_TXPKTBUF_WMAC_LBK   0x45D
#define REG_DATA_SEC            0x483
#define REG_QUEUE_CTRL          0x4C6
#define REG_BAR_MODE_CTRL       0x4CC
#define REG_EDCA_VO_PARAM       0x500
#define REG_EDCA_VI_PARAM       0x504
#define REG_EDCA_BE_PARAM       0x508
#define REG_EDCA_BK_PARAM       0x50C
#define REG_SIFS_CCK            0x514
#define REG_SIFS_OFDM           0x516
#define REG_SPEC_SIFS           0x428
#define REG_RCR                 0x608
#define   RCR_APM               0x00000002
#define   RCR_AM                0x00000004
#define   RCR_AB                0x00000008
#define   RCR_HTC_LOC_CTRL      0x00004000
#define   RCR_APP_PHYSTS        0x10000000
#define   RCR_APP_ICV           0x20000000
#define   RCR_APP_MIC           0x40000000
#define REG_RX_DRVINFO_SZ       0x60F
#define REG_MACID0              0x610
#define REG_MAC_SPEC_SIFS       0x63E
#define REG_ACKTO               0x640
#define REG_NAV_UPPER           0x652
#define REG_WMAC_TRXPTCL_CTL    0x668
#define REG_RXFLTMAP0           0x6A0
#define REG_RXFLTMAP1           0x6A2
#define REG_RXFLTMAP2           0x6A4
#define REG_USB_HRPWM           0xFE58

// Baseband (32-bit registers)
#define BB_FPGA0_RFMOD          0x800
#define   RFMOD_40MHZ           0x00000001
#define   RFMOD_CCK_EN          0x01000000
#define   RFMOD_OFDM_EN         0x02000000
#define BB_RF_ENABLE            0x818
#define BB_HSSI_PARAM1(c)       (0x820 + (c) * 8)
#define   HSSI_PARAM1_PI        0x00000100
#define BB_HSSI_PARAM2(c)       (0x824 + (c) * 8)
#define   HSSI_PARAM2_CCK_HIPWR 0x00000200
#define   HSSI_PARAM2_ADDR_LEN  0x00000400
#define   HSSI_PARAM2_DATA_LEN  0x00000800
#define   HSSI_PARAM2_READ_ADDR_M 0x7F800000
#define   HSSI_PARAM2_READ_ADDR_S 23
#define   HSSI_PARAM2_READ_EDGE 0x80000000
#define BB_LSSI_PARAM(c)        (0x840 + (c) * 4)
#define   LSSI_ADDR_S           20
#define   LSSI_ADDR_M           0x0FF00000
#define   LSSI_DATA_M           0x000FFFFF
#define BB_RFIFACEOE(c)         (0x860 + (c) * 4)
#define BB_RFIFACESW(i)         (0x870 + (i) * 4)
#define BB_LSSI_READBACK(c)     (0x8A0 + (c) * 4)
#define BB_HSPI_READBACK(c)     (0x8B8 + (c) * 4)
#define BB_FPGA1_RFMOD          0x900
#define BB_OFDM0_AGCCORE1_0     0xC50
#define BB_OFDM0_AGCRSSITABLE   0xC78
#define BB_OFDM0_TXPSEUDONOISE  0xCE4

// RF (20-bit registers, through the LSSI)
#define RF_CHNLBW               0x18
#define   RF_CHNLBW_CHNL_M      0x003FF
#define   RF_CHNLBW_BW20        0x00C00

// Queues and pages (r92e.h, three bulk-out endpoints)
#define R92E_TX_PAGE_COUNT      243
#define R92E_PUBQ_NPAGES        222
#define R92E_QUEUE_PAGES        7       // (243 - 222) / 3, high / normal / low
#define R92E_RX_DMA_BUFFER_SIZE 0x3D00

// Firmware
#define FW_HEADER_LEN           32
#define FW_SIGNATURE            0x92E
#define FW_START_ADDR           0x1000
#define FW_PAGE_SIZE            4096

#define RX_DRVINFO_SZ           4       // 32-byte PHY status, in 8-byte units
#define WAIT_MS                 500

//
// Register access
//

static uint32_t rd(struct R92eRadio* r, uint16_t reg, uint16_t length) {
    uint8_t b[4] = { 0, 0, 0, 0 };
    if (r->failed)
        return 0;
    if (!r->bus.read(r->bus.ctx, reg, b, length)) {
        r->failed = true;
        if (r->error == NULL)
            r->error = "register read failed";
        return 0;
    }
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void wr(struct R92eRadio* r, uint16_t reg, uint32_t value, uint16_t length) {
    uint8_t b[4] = { (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24) };
    if (r->failed)
        return;
    if (!r->bus.write(r->bus.ctx, reg, b, length)) {
        r->failed = true;
        if (r->error == NULL)
            r->error = "register write failed";
    }
}

static uint8_t  rd8(struct R92eRadio* r, uint16_t reg)  { return (uint8_t)rd(r, reg, 1); }
static uint16_t rd16(struct R92eRadio* r, uint16_t reg) { return (uint16_t)rd(r, reg, 2); }
static uint32_t rd32(struct R92eRadio* r, uint16_t reg) { return rd(r, reg, 4); }
static void wr8(struct R92eRadio* r, uint16_t reg, uint8_t v)   { wr(r, reg, v, 1); }
static void wr16(struct R92eRadio* r, uint16_t reg, uint16_t v) { wr(r, reg, v, 2); }
static void wr32(struct R92eRadio* r, uint16_t reg, uint32_t v) { wr(r, reg, v, 4); }

static void set8(struct R92eRadio* r, uint16_t reg, uint8_t clear, uint8_t set) {
    wr8(r, reg, (uint8_t)((rd8(r, reg) & ~clear) | set));
}
static void set16(struct R92eRadio* r, uint16_t reg, uint16_t clear, uint16_t set) {
    wr16(r, reg, (uint16_t)((rd16(r, reg) & ~clear) | set));
}
static void set32(struct R92eRadio* r, uint16_t reg, uint32_t clear, uint32_t set) {
    wr32(r, reg, (rd32(r, reg) & ~clear) | set);
}
// One byte of a 32-bit register: rtwn_setbits_1_shift
static void set8_shift(struct R92eRadio* r, uint16_t reg, uint32_t clear, uint32_t set, int shift) {
    set8(r, (uint16_t)(reg + shift), (uint8_t)(clear >> (shift * 8)), (uint8_t)(set >> (shift * 8)));
}

static void sleep_ms(struct R92eRadio* r, uint32_t ms) {
    if (!r->failed && ms != 0)
        r->bus.sleep_ms(ms);
}

// Poll until (reg & mask) == want, up to WAIT_MS
static bool wait32(struct R92eRadio* r, uint16_t reg, uint32_t mask, uint32_t want, const char* what) {
    uint64_t deadline = r->bus.now_ms() + WAIT_MS;
    while (!r->failed) {
        if ((rd32(r, reg) & mask) == want)
            return true;
        if (r->bus.now_ms() >= deadline)
            break;
        r->bus.sleep_ms(1);
    }
    if (r->error == NULL)
        r->error = what;
    r->failed = true;
    return false;
}

// RF registers go through the baseband's serial interface
static void rf_write(struct R92eRadio* r, int chain, uint8_t addr, uint32_t value) {
    set32(r, BB_RF_ENABLE, 0x20000, 0);
    wr32(r, (uint16_t)BB_LSSI_PARAM(chain),
         (((uint32_t)addr << LSSI_ADDR_S) & LSSI_ADDR_M) | (value & LSSI_DATA_M));
    set32(r, BB_RF_ENABLE, 0, 0x20000);
}

static uint32_t rf_read(struct R92eRadio* r, int chain, uint8_t addr) {
    uint32_t v = rd32(r, (uint16_t)BB_HSSI_PARAM2(chain));
    v = (v & ~HSSI_PARAM2_READ_ADDR_M) | ((uint32_t)addr << HSSI_PARAM2_READ_ADDR_S);
    wr32(r, (uint16_t)BB_HSSI_PARAM2(chain), v & ~HSSI_PARAM2_READ_EDGE);
    set32(r, (uint16_t)BB_HSSI_PARAM2(0), HSSI_PARAM2_READ_EDGE, 0);
    set32(r, (uint16_t)BB_HSSI_PARAM2(0), 0, HSSI_PARAM2_READ_EDGE);
    // Each USB round trip is well over the microseconds the chip needs
    if (rd32(r, (uint16_t)BB_HSSI_PARAM1(chain)) & HSSI_PARAM1_PI)
        v = rd32(r, (uint16_t)BB_HSPI_READBACK(chain));
    else
        v = rd32(r, (uint16_t)BB_LSSI_READBACK(chain));
    return v & LSSI_DATA_M;
}

static void rf_setbits(struct R92eRadio* r, int chain, uint8_t addr, uint32_t clear, uint32_t set) {
    rf_write(r, chain, addr, (rf_read(r, chain, addr) & ~clear) | set);
}

//
// Power and firmware
//

static bool power_on(struct R92eRadio* r) {
    if (rd32(r, REG_SYS_CFG) & SYS_CFG_TRP_BT_EN) {
        wr8(r, REG_LDO_SWR_CTRL, 0xC3);
    } else {
        set32(r, REG_LDOV12_CTRL, 0x00100000, 0x00500000);
        wr8(r, REG_LDO_SWR_CTRL, 0x83);
    }

    // Crystal
    set8(r, REG_AFE_PLL_CTRL, AFE_PLL_FREF_SEL, 0);
    set32(r, REG_APE_PLL_CTRL_EXT, 0x00000380, 0);
    set8(r, REG_AFE_PLL_CTRL, 0x40, 0);
    set32(r, REG_APE_PLL_CTRL_EXT, 0x00200000, 0);

    // WL suspend on, then HW power down, SW LPS and WL suspend off
    set8_shift(r, REG_APS_FSMCO, APS_AFSM_HSUS | APS_AFSM_PCIE, 0, 1);
    set8_shift(r, REG_APS_FSMCO, APS_APFM_RSM | APS_AFSM_HSUS | APS_AFSM_PCIE | APS_APDM_HPDN, 0, 1);
    if (!wait32(r, REG_APS_FSMCO, APS_SUS_HOST, APS_SUS_HOST, "chip did not power up"))
        return false;

    // Release WLON reset, power the MAC on
    set8_shift(r, REG_APS_FSMCO, 0, APS_RDY_MACON, 2);
    set8_shift(r, REG_APS_FSMCO, 0, APS_APFM_ONMAC, 1);
    if (!wait32(r, REG_APS_FSMCO, APS_APFM_ONMAC, 0, "MAC did not power on"))
        return false;

    // MAC DMA, WMAC, scheduler (no hardware encryption)
    wr16(r, REG_CR, 0);
    set16(r, REG_CR, 0, CR_HCI_TXDMA_EN | CR_TXDMA_EN | CR_HCI_RXDMA_EN | CR_RXDMA_EN |
                        CR_PROTOCOL_EN | CR_SCHEDULE_EN | CR_CALTMR_EN);
    return !r->failed;
}

static void fw_reset(struct R92eRadio* r) {
    set8(r, REG_RSV_CTRL + 1, 0x01, 0);
    set8_shift(r, REG_SYS_FUNC_EN, FEN_CPUEN, 0, 1);
    set8(r, REG_RSV_CTRL + 1, 0, 0x01);
    set8_shift(r, REG_SYS_FUNC_EN, 0, FEN_CPUEN, 1);
}

static bool fw_write_page(struct R92eRadio* r, uint32_t page, const uint8_t* data, uint32_t length) {
    uint32_t reg = rd32(r, REG_MCUFWDL);
    wr32(r, REG_MCUFWDL, (reg & ~MCUFWDL_PAGE_M) | ((page << MCUFWDL_PAGE_S) & MCUFWDL_PAGE_M));

    uint16_t offset = FW_START_ADDR;
    while (length > 0 && !r->failed) {
        uint32_t n = length > RTW_BUS_BLOCK_MAX ? RTW_BUS_BLOCK_MAX : (length > 4 ? 4 : 1);
        if (!r->bus.write(r->bus.ctx, offset, data, (uint16_t)n)) {
            r->failed = true;
            r->error = "firmware write failed";
        }
        offset = (uint16_t)(offset + n);
        data += n;
        length -= n;
    }
    return !r->failed;
}

// The 8051's code. The image is a file on disk: only its size and header
// are taken on trust, and it runs on the adapter's own CPU, not this one.
static bool fw_load(struct R92eRadio* r, const uint8_t* fw, uint32_t length) {
    if (length < FW_HEADER_LEN || length > R92E_FW_MAX)
        return false;
    if (((uint32_t)(fw[0] | (fw[1] << 8)) >> 4) == FW_SIGNATURE) {
        fw += FW_HEADER_LEN;
        length -= FW_HEADER_LEN;
    }
    // Pages are numbered in 3 bits
    if (length == 0 || length > 8 * FW_PAGE_SIZE)
        return false;

    if (rd8(r, REG_MCUFWDL) & MCUFWDL_RAM_DL_SEL) {
        wr8(r, REG_MCUFWDL, 0);
        fw_reset(r);
    }
    set8(r, REG_MCUFWDL, 0, MCUFWDL_EN);
    set8_shift(r, REG_MCUFWDL, MCUFWDL_ROM_DLEN, 0, 2);

    bool ok = false;
    for (int tries = 0; tries < 3 && !ok && !r->failed; tries++) {
        set8(r, REG_MCUFWDL, 0, MCUFWDL_CHKSUM_RPT);
        uint32_t page = 0;
        for (uint32_t done = 0; done < length && !r->failed; done += FW_PAGE_SIZE, page++) {
            uint32_t n = length - done < FW_PAGE_SIZE ? length - done : FW_PAGE_SIZE;
            fw_write_page(r, page, fw + done, n);
        }
        // Checksum report: up to 250 ms
        for (int i = 0; i < 25 && !r->failed && !ok; i++) {
            ok = (rd32(r, REG_MCUFWDL) & MCUFWDL_CHKSUM_RPT) != 0;
            if (!ok)
                sleep_ms(r, 10);
        }
    }
    set8(r, REG_MCUFWDL, MCUFWDL_EN, 0);
    if (!ok || r->failed)
        return false;

    set32(r, REG_MCUFWDL, MCUFWDL_WINTINI_RDY, MCUFWDL_RDY);
    fw_reset(r);
    for (int i = 0; i < 20 && !r->failed; i++) {
        if (rd32(r, REG_MCUFWDL) & MCUFWDL_WINTINI_RDY)
            return true;
        sleep_ms(r, 10);
    }
    return false;
}

//
// MAC, DMA, baseband, RF
//

static void mac_init(struct R92eRadio* r) {
    for (uint32_t i = 0; i < sizeof(r92e_mac_init) / sizeof(r92e_mac_init[0]) && !r->failed; i++)
        wr8(r, r92e_mac_init[i][0], (uint8_t)r92e_mac_init[i][1]);
}

static bool dma_init(struct R92eRadio* r) {
    // Link list table
    set32(r, REG_AUTO_LLT, 0, AUTO_LLT_INIT);
    if (!wait32(r, REG_AUTO_LLT, AUTO_LLT_INIT, 0, "LLT init timed out"))
        return false;

    // Pages per queue: public, then high / normal / low
    wr8(r, REG_RQPN_NPQ, R92E_QUEUE_PAGES);
    wr32(r, REG_RQPN, ((uint32_t)R92E_PUBQ_NPAGES << 16) | ((uint32_t)R92E_QUEUE_PAGES << 8) |
                      R92E_QUEUE_PAGES | RQPN_LD);

    uint8_t boundary = R92E_TX_PAGE_COUNT + 1;
    wr8(r, REG_TXPKTBUF_BCNQ_BDNY, boundary);
    wr8(r, REG_TXPKTBUF_MGQ_BDNY, boundary);
    wr8(r, REG_TXPKTBUF_WMAC_LBK, boundary);
    wr8(r, REG_TRXFF_BNDY, boundary);
    wr8(r, REG_TDECTRL + 1, boundary);

    set16(r, REG_TRXDMA_CTRL, TRXDMA_QMAP_M, TRXDMA_QMAP_3EP);
    wr16(r, REG_TRXFF_BNDY + 2, R92E_RX_DMA_BUFFER_SIZE - 1);
    return !r->failed;
}

static void bb_init(struct R92eRadio* r) {
    set16(r, REG_SYS_FUNC_EN, 0, FEN_USBA | FEN_USBD);
    set16(r, REG_SYS_FUNC_EN, 0, FEN_BBRSTB | FEN_BB_GLB_RST | FEN_DIO_RF);
    wr8(r, REG_RF_CTRL, RF_CTRL_EN | RF_CTRL_RSTB | RF_CTRL_SDMRSTB);

    for (uint32_t i = 0; i < sizeof(r92e_bb_regs) / sizeof(r92e_bb_regs[0]) && !r->failed; i++)
        wr32(r, r92e_bb_regs[i], r92e_bb_vals[i]);
    for (uint32_t i = 0; i < sizeof(r92e_agc_vals) / sizeof(r92e_agc_vals[0]) && !r->failed; i++)
        wr32(r, BB_OFDM0_AGCRSSITABLE, r92e_agc_vals[i]);

    r->cck_hipwr = (rd32(r, (uint16_t)BB_HSSI_PARAM2(0)) & HSSI_PARAM2_CCK_HIPWR) != 0;

    wr32(r, BB_OFDM0_AGCCORE1_0, 0x00040022);
    wr32(r, BB_OFDM0_AGCCORE1_0, 0x00040020);

    // Crystal trim from the efuse
    uint32_t cap = r->crystalcap & 0x3F;
    uint32_t reg = rd32(r, REG_R92E_XTAL_CTRL);
    reg = (reg & ~R92E_XTAL_ADDR_M) | (((cap | (cap << 6)) << R92E_XTAL_ADDR_S) & R92E_XTAL_ADDR_M);
    wr32(r, REG_R92E_XTAL_CTRL, reg);
    wr32(r, REG_AFE_XTAL_CTRL, 0x000F81FB);
}

static void rf_chain(struct R92eRadio* r, const uint8_t* regs, const uint32_t* vals, uint32_t count, int chain) {
    for (uint32_t i = 0; i < count && !r->failed; i++) {
        if (regs[i] > 0xF8) {
            // A delay, in microseconds
            sleep_ms(r, (vals[i] + 999) / 1000);
            continue;
        }
        rf_write(r, chain, regs[i], vals[i]);
    }
}

static void rf_init(struct R92eRadio* r) {
    for (int chain = 0; chain < 2 && !r->failed; chain++) {
        int off = (chain % 2) * 16;
        uint32_t reg = rd32(r, (uint16_t)BB_RFIFACESW(0));
        uint32_t type = (reg >> off) & 0x10;

        set32(r, (uint16_t)BB_RFIFACEOE(chain), 0, 0x100000);
        set32(r, (uint16_t)BB_RFIFACEOE(chain), 0, 0x10);
        set32(r, (uint16_t)BB_HSSI_PARAM2(chain), HSSI_PARAM2_ADDR_LEN, 0);
        set32(r, (uint16_t)BB_HSSI_PARAM2(chain), HSSI_PARAM2_DATA_LEN, 0);

        if (chain == 0)
            rf_chain(r, r92e_rf0_regs, r92e_rf0_vals, sizeof(r92e_rf0_regs), 0);
        else
            rf_chain(r, r92e_rf1_regs, r92e_rf1_vals, sizeof(r92e_rf1_regs), 1);

        // As rtwn does it (the expression is theirs, kept as is)
        reg = rd32(r, (uint16_t)BB_RFIFACESW(0));
        reg &= ~(0x10u << off) | (type << off);
        wr32(r, (uint16_t)BB_RFIFACESW(0), reg);

        r->rf_chnlbw[chain] = rf_read(r, chain, RF_CHNLBW);
    }
    set32(r, BB_FPGA0_RFMOD, 0, RFMOD_CCK_EN);
    set32(r, BB_FPGA0_RFMOD, 0, RFMOD_OFDM_EN);
}

// Receive management frames from every network (a scan): beacons and
// probe responses; no control or data frames
static void rx_filter(struct R92eRadio* r) {
    wr16(r, REG_RXFLTMAP0, 0x7F7F & ~((1 << 0) | (1 << 2) | (1 << 4)));   // Not (re)assoc / probe requests
    wr16(r, REG_RXFLTMAP1, 0x0000);
    wr16(r, REG_RXFLTMAP2, 0x0000);
    wr32(r, REG_RCR, RCR_AM | RCR_AB | RCR_APM | RCR_HTC_LOC_CTRL |
                     RCR_APP_PHYSTS | RCR_APP_ICV | RCR_APP_MIC);
}

bool r92e_set_channel(struct R92eRadio* r, uint8_t channel) {
    if (channel < 1 || channel > 13 || r->failed)
        return false;
    for (int i = 0; i < 2; i++)
        rf_write(r, i, RF_CHNLBW, (r->rf_chnlbw[0] & ~RF_CHNLBW_CHNL_M) | channel);

    // 20 MHz
    set16(r, REG_WMAC_TRXPTCL_CTL, 0x180, 0);
    wr8(r, REG_DATA_SEC, 0);
    set32(r, BB_FPGA0_RFMOD, RFMOD_40MHZ, 0);
    set32(r, BB_FPGA1_RFMOD, RFMOD_40MHZ, 0);
    for (int i = 0; i < 2; i++)
        rf_setbits(r, i, RF_CHNLBW, RF_CHNLBW_BW20, 0xC00);
    set32(r, BB_OFDM0_TXPSEUDONOISE, 0xC0000000, 0);

    sleep_ms(r, 10);
    if (!r->failed)
        r->channel = channel;
    return !r->failed;
}

bool r92e_radio_on(struct R92eRadio* r, const uint8_t* fw, uint32_t fw_length) {
    r->failed = false;
    r->error = NULL;
    r->fw_loaded = false;

    if (!power_on(r))
        return false;

    if (fw != NULL)
        r->fw_loaded = fw_load(r, fw, fw_length);
    if (r->failed)
        return false;

    mac_init(r);
    if (!dma_init(r))
        return false;

    set8_shift(r, REG_TXDMA_OFFSET_CHK, 0, TXDMA_DROP_DATA_EN, 1);
    wr8(r, REG_RX_DRVINFO_SZ, RX_DRVINFO_SZ);
    wr32(r, REG_HIMR, 0);
    wr32(r, REG_HIMRE, 0);
    set8(r, REG_MSR, 0x03, 0);      // No link
    for (int i = 0; i < 6; i++)
        wr8(r, (uint16_t)(REG_MACID0 + i), r->mac[i]);
    rx_filter(r);

    wr16(r, REG_RL, 0x3030);
    wr16(r, REG_SPEC_SIFS, 0x100A);
    wr16(r, REG_MAC_SPEC_SIFS, 0x100A);
    wr16(r, REG_SIFS_CCK, 0x100A);
    wr16(r, REG_SIFS_OFDM, 0x100A);
    wr32(r, REG_EDCA_BE_PARAM, 0x005EA42B);
    wr32(r, REG_EDCA_BK_PARAM, 0x0000A44F);
    wr32(r, REG_EDCA_VI_PARAM, 0x005EA324);
    wr32(r, REG_EDCA_VO_PARAM, 0x002FA226);
    set8(r, REG_FWHW_TXQ_CTRL, 0, FWHW_AMPDU_RTY_NEW);
    wr8(r, REG_ACKTO, 0x40);

    // USB receive: no aggregation, so each frame comes in its own transfer
    set8(r, REG_RXDMA_PRO, 0x20, 0x1E);

    set8(r, REG_CR, 0, CR_MACTXEN | CR_MACRXEN);

    bb_init(r);
    rf_init(r);
    if (r->failed)
        return false;
    if (!r92e_set_channel(r, 1))
        return false;

    wr8(r, REG_HWSEQ_CTRL, 0xFF);
    wr32(r, REG_BAR_MODE_CTRL, 0x0201FFFF);
    wr8(r, REG_NAV_UPPER, 0);
    set8(r, REG_GPIO_MUXCFG, GPIO_MUXCFG_ENBT, 0);
    set8(r, REG_QUEUE_CTRL, 0x08, 0);
    wr8(r, REG_ACLK_MON, 0);
    wr8(r, REG_USB_HRPWM, 0);

    if (r->failed && r->error == NULL)
        r->error = "register access failed";
    return !r->failed;
}
