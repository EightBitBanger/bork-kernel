#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "kshim.h"
#include "rtw8192e.h"
#include "rtw_devices.h"
#include "rtw_rx.h"

//
// Realtek USB Wi-Fi adapters: a thin driver
//
//   RTL8811AU / RTL8821AU  802.11ac "AC600" (TP-Link Archer T2U, Edimax EW-7811UTC, ...)
//   RTL8811CU / RTL8821CU  802.11ac "AC600" (most cheap AC600 sticks since ~2019)
//   RTL8192EU              802.11n 2x2 "300M" (TP-Link TL-WN822N v4, D-Link DWA-131 E1, ...)
//
// A driver file, sys/drivers/rtw-usb: the kernel loads it when one of these
// sticks is plugged in (driver/autoload.c, rtw_devices.h), and has none of
// this built in. The wlanN names, beacon parsing and the IP stack stay in
// the kernel and are called through the driver table (kshim.h).
//
// What it does: binds the adapter's vendor interface, checks its
// descriptors, reads the chip's version register, reads the efuse (the
// chip's one-time ROM) for the MAC address, and offers /dev/wlanN (needs
// SYS_SEC_NET, see wlan.h) to report all that. It also registers a network
// interface of the same name with the IP stack (net/net.h), which stays
// without link: the stack, DHCP included, starts on it the moment
// netif_set_link(true) is called.
//
// RTL8192EU only: it brings the radio up (rtw8192e.c) when a scan is asked
// for and listens on channels 1-13 for beacons (a passive scan; nothing is
// ever transmitted). The firmware, <home>/sys/firmware/rtl8192eu_nic.bin,
// is loaded when present; receiving works without it.
//
// What it does not do yet: transmit, associate, WPA2, or move data frames;
// and nothing past identification on the RTL8821A / RTL8821C.
//
// Slow work (the efuse, radio bring-up, one channel of a scan) runs in
// poll() on the "drivers" thread, one step per call, so a scan takes about
// WLAN_CHANNEL_MAX polls.
//
// RTL8821CU sticks first show up as a virtual CD-ROM (0bda:1a2b, "ZeroCD")
// holding the Windows driver. This driver ejects it, which makes the stick
// drop off the bus and come back as the network adapter (what
// usb_modeswitch does on Linux).
//
// Everything the adapter returns is untrusted: transfer lengths are
// checked, register values only ever index tables after a range check, and
// the efuse walk is bounded on both the bytes it reads and the map it
// writes.
//

#define RTW_MAX_ADAPTERS        4
#define RTW_POLL_MS             250

// USB vendor request: read or write 1, 2 or 4 bytes of a chip register
#define RTW_REQ                 0x05
#define RTW_REQ_READ            (USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE)
#define RTW_REQ_WRITE           (USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE)

// Registers (the same addresses on both families)
#define REG_SYS_FUNC_EN         0x0002
#define   FEN_ELDR              (1U << 12)  // Efuse loader
#define REG_SYS_CLKR            0x0008
#define   CLKR_ANA8M            (1U << 1)
#define   CLKR_LOADER_CLK_EN    (1U << 5)
#define REG_EFUSE_CTRL          0x0030
#define   EFUSE_FLAG            (1U << 31)  // Read: set when data is valid
#define   EFUSE_ADDR_SHIFT      8
#define   EFUSE_ADDR_MASK       0x3FFU
#define   EFUSE_KEEP_MASK       0x7FFC0000U // Bits 18-30: settings to leave alone
#define REG_EFUSE_ACCESS        0x00CF
#define   EFUSE_ACCESS_ON       0x69
#define   EFUSE_ACCESS_OFF      0x00
#define REG_SYS_CFG             0x00F0
#define   SYS_CFG_CUT_SHIFT     12
#define   SYS_CFG_CUT_MASK      0xFU
#define REG_SYS_CFG2            0x00FC      // RTL8821C family: chip ID

#define RTW_CHIP_ID_8821C       0x09

// Efuse: 512 bytes physical, decoded into a 512-byte logical map
#define EFUSE_PHYS_SIZE         512
#define EFUSE_LOG_SIZE          512
#define EFUSE_MAC_8821          0x107       // USB parts of RTL8821A / RTL8821C
#define EFUSE_MAC_8192E         0x0D7       // RTL8192EU
#define EFUSE_BYTE_TRIES        100         // Register polls per byte
#define EFUSE_DEADLINE_MS       10000       // The whole read

// ZeroCD eject: one Bulk-Only command, SCSI START STOP UNIT with LoEj
#define ZEROCD_TIMEOUT_MS       1000
#define CBW_LENGTH              31
#define CSW_LENGTH              13

// Families (WLAN_FAMILY_* in wlan.h) plus the CD-ROM mode
#define RTW_ZEROCD              0x80

struct RtwId {
    uint16_t vendor;
    uint16_t product;
    uint8_t  family;
};

#define RTW_ID_ROW(v, p, f)     { (v), (p), (f) },
static const struct RtwId rtw_ids[] = {
    RTW_DEVICES(RTW_ID_ROW)
};

// The adapters' own interface is vendor specific (class 0xFF); a
// Bluetooth interface on the same stick (8821AU / 8821CU) is left alone.
// RTL8192EU has the same register and efuse access, only its efuse layout
// differs.
#define RTW_MATCH_ROW(v, p, f)  \
    { .bus = BUS_USB, .vendor = (v), .product = (p), .class_code = USB_CLASS_VENDOR, \
      .subclass = MATCH_ANY, .protocol = MATCH_ANY },
static const struct DeviceMatch rtw_match[] = {
    RTW_DEVICES(RTW_MATCH_ROW)
    { .bus = BUS_USB, .vendor = RTW_ZEROCD_VENDOR, .product = RTW_ZEROCD_PRODUCT,
      .class_code = USB_CLASS_MASS_STORAGE, .subclass = MATCH_ANY, .protocol = 0x50 },
    { 0 }
};

struct RtwAdapter {
    struct Device*     dev;
    struct WlanDev*    wlan;        // /dev/wlanN. NULL in CD-ROM mode
    struct NetIf*      netif;       // NULL in CD-ROM mode, or if the stack had no room
    uint8_t  index;                 // Slot in adapters[]
    uint8_t  family;                // WLAN_FAMILY_* or RTW_ZEROCD
    uint8_t  state;                 // WLAN_STATE_*
    uint8_t  speed;
    uint16_t vendor;
    uint16_t product;
    uint32_t sys_cfg;
    uint8_t  chip_id;
    uint8_t  mac[6];
    bool     mac_valid;

    uint8_t  crystalcap;            // RTL8192E crystal trim (efuse)

    bool     efuse_pending;         // poll() still has to read the efuse
    volatile bool busy;             // poll() is using it without the lock
    bool     removing;

    // Scanning. radio / rx_pipe / rx_buffer belong to poll() (only touched
    // while busy, or by remove() once busy is clear).
    struct R92eRadio* radio;        // NULL until first brought up
    bool     radio_up;
    struct UsbPipe* rx_pipe;
    uint8_t* rx_buffer;             // USB_PIPE_MAX
    uint8_t  scan_state;            // WLAN_SCAN_*
    uint8_t  scan_channel;          // Next channel to listen to (RUNNING)
    uint8_t  scan_count;
    struct WlanBss scan[WLAN_SCAN_MAX];
};

// adapters[] and the fields above, except while `busy`. Never held while
// taking another lock: ioctl takes it with the VFS lock held.
static mutex_t rtw_lock;

const struct KernelApi* kapi;

// The kernel log (COM1), "drv: rtw-usb: ...". Not the screen: a stick may
// be plugged in while the desktop owns it.
#define rtw_log(...)    kapi->log("rtw-usb: " __VA_ARGS__)
static bool rtw_ready = false;
static struct RtwAdapter* adapters[RTW_MAX_ADAPTERS];

//
// Register access (no lock held: these go through the controller lock)
//

static bool rtw_read(const struct Device* dev, uint16_t reg, void* out, uint16_t length) {
    return usb_dev_control(dev, RTW_REQ_READ, RTW_REQ, reg, 0, out, length) == (int)length;
}

static bool rtw_write(const struct Device* dev, uint16_t reg, const void* data, uint16_t length) {
    uint8_t buf[RTW_BUS_BLOCK_MAX];
    if (length == 0 || length > sizeof(buf))
        return false;
    memcpy(buf, data, length);
    return usb_dev_control(dev, RTW_REQ_WRITE, RTW_REQ, reg, 0, buf, length) == (int)length;
}

static bool rtw_read8(const struct Device* dev, uint16_t reg, uint8_t* out) {
    return rtw_read(dev, reg, out, 1);
}

static bool rtw_read16(const struct Device* dev, uint16_t reg, uint16_t* out) {
    uint8_t b[2];
    if (!rtw_read(dev, reg, b, 2))
        return false;
    *out = (uint16_t)(b[0] | (b[1] << 8));
    return true;
}

static bool rtw_read32(const struct Device* dev, uint16_t reg, uint32_t* out) {
    uint8_t b[4];
    if (!rtw_read(dev, reg, b, 4))
        return false;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}

static bool rtw_write8(const struct Device* dev, uint16_t reg, uint8_t value) {
    return rtw_write(dev, reg, &value, 1);
}

static bool rtw_write16(const struct Device* dev, uint16_t reg, uint16_t value) {
    uint8_t b[2] = { (uint8_t)value, (uint8_t)(value >> 8) };
    return rtw_write(dev, reg, b, 2);
}

static bool rtw_write32(const struct Device* dev, uint16_t reg, uint32_t value) {
    uint8_t b[4] = { (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24) };
    return rtw_write(dev, reg, b, 4);
}

//
// Efuse (from poll(), no lock held)
//

struct EfuseReader {
    const struct Device* dev;
    uint32_t ctrl;                  // REG_EFUSE_CTRL bits to keep
    uint32_t next;                  // Next physical address
    uint64_t deadline;
    bool     failed;
};

// Turn the efuse loader on (vendor driver: Efuse_PowerSwitch, read only)
static bool efuse_power(const struct Device* dev, bool on) {
    if (!on)
        return rtw_write8(dev, REG_EFUSE_ACCESS, EFUSE_ACCESS_OFF);

    if (!rtw_write8(dev, REG_EFUSE_ACCESS, EFUSE_ACCESS_ON))
        return false;

    uint16_t value;
    if (!rtw_read16(dev, REG_SYS_FUNC_EN, &value))
        return false;
    if (!(value & FEN_ELDR) && !rtw_write16(dev, REG_SYS_FUNC_EN, (uint16_t)(value | FEN_ELDR)))
        return false;

    if (!rtw_read16(dev, REG_SYS_CLKR, &value))
        return false;
    uint16_t want = CLKR_LOADER_CLK_EN | CLKR_ANA8M;
    if ((value & want) != want && !rtw_write16(dev, REG_SYS_CLKR, (uint16_t)(value | want)))
        return false;
    return true;
}

// The next physical efuse byte. Sets r->failed (and returns 0xFF, the
// "end" marker) on an error, a timeout, or past the end of the efuse.
static uint8_t efuse_next(struct EfuseReader* r) {
    if (r->failed || r->next >= EFUSE_PHYS_SIZE || timer_get_ms() > r->deadline) {
        r->failed = true;
        return 0xFF;
    }

    uint32_t address = r->next++;
    if (!rtw_write32(r->dev, REG_EFUSE_CTRL, r->ctrl | ((address & EFUSE_ADDR_MASK) << EFUSE_ADDR_SHIFT))) {
        r->failed = true;
        return 0xFF;
    }
    for (int tries = 0; tries < EFUSE_BYTE_TRIES; tries++) {
        uint32_t value;
        if (!rtw_read32(r->dev, REG_EFUSE_CTRL, &value))
            break;
        if (value & EFUSE_FLAG)
            return (uint8_t)value;
    }
    r->failed = true;
    return 0xFF;
}

// Walk the physical efuse into the logical map. The efuse is a log of
// patches: a header names an 8-byte section of the map and which of its 4
// words follow. Both the bytes read and the map offsets written are bounded
// whatever the headers say. True if the walk reached the end marker.
static bool efuse_read_map(const struct Device* dev, uint8_t* map) {
    memset(map, 0xFF, EFUSE_LOG_SIZE);

    struct EfuseReader r = { .dev = dev, .deadline = timer_get_ms() + EFUSE_DEADLINE_MS };
    uint32_t ctrl;
    if (!rtw_read32(dev, REG_EFUSE_CTRL, &ctrl))
        return false;
    r.ctrl = ctrl & EFUSE_KEEP_MASK;

    while (!r.failed) {
        uint8_t hdr1 = efuse_next(&r);
        if (hdr1 == 0xFF)
            break;      // End of what was ever written (or a failure)

        uint32_t section;
        uint8_t  words;     // Bit clear: that word follows
        if ((hdr1 & 0x1F) == 0x0F) {
            // Two-byte header: sections 16 and up
            uint8_t hdr2 = efuse_next(&r);
            if (hdr2 == 0xFF)
                break;
            if ((hdr2 & 0x0F) == 0x0F)
                continue;   // An entry with no words: nothing follows
            section = ((uint32_t)(hdr2 & 0xF0) >> 1) | ((uint32_t)(hdr1 >> 5) & 0x07);
            words   = hdr2 & 0x0F;
        } else {
            section = hdr1 >> 4;
            words   = hdr1 & 0x0F;
        }

        for (uint32_t w = 0; w < 4 && !r.failed; w++) {
            if (words & (1U << w))
                continue;
            uint8_t lo = efuse_next(&r);
            uint8_t hi = efuse_next(&r);
            // Sections can name up to 127 * 8; only the map's own are kept
            uint32_t offset = section * 8 + w * 2;
            if (!r.failed && offset + 2 <= EFUSE_LOG_SIZE) {
                map[offset]     = lo;
                map[offset + 1] = hi;
            }
        }
    }

    // Running off the end of the efuse or the clock is a failure; an 0xFF
    // header is the normal end
    return !r.failed;
}

static bool mac_usable(const uint8_t* mac) {
    bool zero = true, ones = true;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0x00) zero = false;
        if (mac[i] != 0xFF) ones = false;
    }
    return !zero && !ones && !(mac[0] & 0x01);  // Not multicast either
}

// Read the MAC address from the efuse. False if it can't be read or is not
// a usable unicast address.
static bool rtw_read_mac(const struct Device* dev, uint8_t family, uint8_t* mac, uint8_t* crystalcap) {
    uint8_t* map = (uint8_t*)malloc(EFUSE_LOG_SIZE);
    if (map == NULL)
        return false;

    bool ok = efuse_power(dev, true) && efuse_read_map(dev, map);
    efuse_power(dev, false);

    if (ok) {
        // Both offsets + 6 lie inside the map (EFUSE_LOG_SIZE)
        uint32_t offset = family == WLAN_FAMILY_RTL8192E ? EFUSE_MAC_8192E : EFUSE_MAC_8821;
        memcpy(mac, &map[offset], 6);
        ok = mac_usable(mac);
        // Blank (0xFF) means "use the default"
        uint8_t cap = map[R92E_EFUSE_CRYSTALCAP];
        if (family == WLAN_FAMILY_RTL8192E && cap != 0xFF)
            *crystalcap = cap;
    }
    if (!ok)
        memset(mac, 0, 6);
    free(map);
    return ok;
}

//
// /dev/wlanN (wlan_dev.c routes it here)
//

static const char* rtw_chip_name(const struct RtwAdapter* a) {
    if (a->family == WLAN_FAMILY_RTL8821A)
        return "RTL8811/21AU";
    if (a->family == WLAN_FAMILY_RTL8821C)
        return a->chip_id == RTW_CHIP_ID_8821C ? "RTL8821CU" : "RTL8821CU?";
    if (a->family == WLAN_FAMILY_RTL8192E)
        return "RTL8192EU";
    return "?";
}

// rtw_lock held. `info` was zeroed by the caller.
static void rtw_fill_info(const struct RtwAdapter* a, struct WlanInfo* info) {
    info->version   = WLAN_INFO_VERSION;
    info->vendor    = a->vendor;
    info->product   = a->product;
    info->family    = a->family;
    info->state     = a->state;
    info->usb_speed = a->speed;
    info->mac_valid = a->mac_valid ? 1 : 0;
    if (a->mac_valid)
        memcpy(info->mac, a->mac, 6);
    info->chip_cut  = (uint8_t)((a->sys_cfg >> SYS_CFG_CUT_SHIFT) & SYS_CFG_CUT_MASK);
    info->chip_id   = a->chip_id;
    info->sys_cfg   = a->sys_cfg;
    strncpy(info->chip, rtw_chip_name(a), sizeof(info->chip) - 1);
}

// rtw_lock held
static int32_t rtw_scan_start_locked(struct RtwAdapter* a) {
    if (a->removing)
        return SYS_ENOENT;
    if (a->family != WLAN_FAMILY_RTL8192E)
        return SYS_ENOTTY;
    if (a->efuse_pending || a->state == WLAN_STATE_PROBED)
        return SYS_EAGAIN;
    if (a->scan_state == WLAN_SCAN_RUNNING)
        return SYS_EBUSY;
    a->scan_state   = WLAN_SCAN_RUNNING;
    a->scan_channel = 1;
    a->scan_count   = 0;
    memset(a->scan, 0, sizeof(a->scan));
    return SYS_OK;
}

// rtw_lock held. `out` was zeroed by the caller.
static void rtw_fill_results(const struct RtwAdapter* a, struct WlanScanResults* out) {
    out->version  = WLAN_SCAN_VERSION;
    out->state    = a->scan_state;
    out->channel  = a->scan_state == WLAN_SCAN_RUNNING ? a->scan_channel : 0;
    out->count    = a->scan_count <= WLAN_SCAN_MAX ? a->scan_count : WLAN_SCAN_MAX;
    out->firmware = (a->radio != NULL && a->radio->fw_loaded) ? 1 : 0;
    memcpy(out->bss, a->scan, sizeof(struct WlanBss) * out->count);
}

// Unplugged: remove() is tearing it down and the handle is about to go
static int32_t rtw_wlan_info(void* ctx, struct WlanInfo* out) {
    struct RtwAdapter* a = (struct RtwAdapter*)ctx;
    mutex_lock(&rtw_lock);
    bool gone = a->removing;
    if (!gone)
        rtw_fill_info(a, out);
    mutex_unlock(&rtw_lock);
    return gone ? SYS_ENOENT : SYS_OK;
}

static int32_t rtw_wlan_scan_start(void* ctx) {
    struct RtwAdapter* a = (struct RtwAdapter*)ctx;
    mutex_lock(&rtw_lock);
    int32_t result = rtw_scan_start_locked(a);
    mutex_unlock(&rtw_lock);
    return result;
}

static int32_t rtw_wlan_scan_results(void* ctx, struct WlanScanResults* out) {
    struct RtwAdapter* a = (struct RtwAdapter*)ctx;
    mutex_lock(&rtw_lock);
    bool gone = a->removing;
    if (!gone)
        rtw_fill_results(a, out);
    mutex_unlock(&rtw_lock);
    return gone ? SYS_ENOENT : SYS_OK;
}

static const struct WlanOps rtw_wlan_ops = {
    .info         = rtw_wlan_info,
    .scan_start   = rtw_wlan_scan_start,
    .scan_results = rtw_wlan_scan_results,
};

// Network interface. No data path yet: the link never comes up, so the
// stack never sends, but say so plainly if it tries.
static int rtw_net_transmit(struct NetIf* ifp, const void* frame, uint32_t length) {
    (void)ifp; (void)frame; (void)length;
    return NET_ERR_DOWN;
}

static const struct NetIfOps rtw_net_ops = {
    .transmit = rtw_net_transmit,
};

//
// CD-ROM mode
//

// Send the eject. The stick drops off the bus whether or not the status
// comes back, so the status is read only to finish the command politely.
static bool rtw_zerocd_eject(struct Device* dev, const struct UsbInterface* itf) {
    struct UsbPipe* out = usb_pipe_open_bulk(dev, itf->bulk_out_endpoint, false);
    struct UsbPipe* in  = usb_pipe_open_bulk(dev, itf->bulk_in_endpoint, true);
    bool sent = false;

    if (out != NULL && in != NULL) {
        // CBW "USBC", tag, no data, LUN 0, 6-byte START STOP UNIT (LoEj)
        uint8_t cbw[CBW_LENGTH] = {
            0x55, 0x53, 0x42, 0x43,  0x52, 0x54, 0x57, 0x31,
            0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x06,
            0x1B, 0x00, 0x00, 0x00, 0x02, 0x00,
        };
        sent = usb_pipe_transfer(out, cbw, sizeof(cbw), ZEROCD_TIMEOUT_MS) == (int)sizeof(cbw);
        if (sent) {
            uint8_t csw[CSW_LENGTH];
            usb_pipe_transfer(in, csw, sizeof(csw), ZEROCD_TIMEOUT_MS);
        }
    }
    usb_pipe_close(in);
    usb_pipe_close(out);
    return sent;
}

//
// Driver
//

static uint8_t rtw_family_of(uint16_t vendor, uint16_t product) {
    if (vendor == RTW_ZEROCD_VENDOR && product == RTW_ZEROCD_PRODUCT)
        return RTW_ZEROCD;
    for (uint32_t i = 0; i < sizeof(rtw_ids) / sizeof(rtw_ids[0]); i++) {
        if (rtw_ids[i].vendor == vendor && rtw_ids[i].product == product)
            return rtw_ids[i].family;
    }
    return 0;
}

// Is a bulk max packet size one the speed allows (USB 2.0 5.8.3, 3.x 9.6.6)
static bool rtw_bulk_size_ok(uint8_t speed, uint16_t size) {
    switch (speed) {
        case USB_SPEED_FULL:  return size == 8 || size == 16 || size == 32 || size == 64;
        case USB_SPEED_HIGH:  return size == 512;
        case USB_SPEED_SUPER: return size == 1024;
        default:              return false;     // Low speed has no bulk endpoints
    }
}

static void rtw_print_ids(const char* what, const struct RtwAdapter* a) {
    rtw_log("%04x:%04x %s", (unsigned)a->vendor, (unsigned)a->product, what);
}

static int rtw_probe(struct Device* dev) {
    if (!rtw_ready)
        return PROBE_FAILED;

    uint8_t speed = kapi->usb_speed(dev);
    const struct UsbInterface* itf = usb_dev_interface(dev);
    if (speed == 0 || itf == NULL)
        return PROBE_NOT_MINE;
    uint8_t family = rtw_family_of(dev->vendor, dev->product);
    if (family == 0)
        return PROBE_NOT_MINE;

    // The descriptors are the device's say-so: a bulk pair of sizes its
    // speed allows, or nothing here talks to it
    if (itf->bulk_in_endpoint == 0 || itf->bulk_out_endpoint == 0 ||
        !rtw_bulk_size_ok(speed, itf->bulk_in_max_packet) ||
        !rtw_bulk_size_ok(speed, itf->bulk_out_max_packet))
        return PROBE_FAILED;

    struct RtwAdapter* a = (struct RtwAdapter*)malloc(sizeof(struct RtwAdapter));
    if (a == NULL)
        return PROBE_FAILED;
    memset(a, 0, sizeof(*a));
    a->dev     = dev;
    a->family  = family;
    a->speed   = speed;
    a->vendor  = dev->vendor;
    a->product = dev->product;
    a->crystalcap = 0x20;           // RTL8192E default, until the efuse says

    if (family == RTW_ZEROCD) {
        bool ejected = rtw_zerocd_eject(dev, itf);
        rtw_print_ids(ejected ? "CD-ROM mode: ejected, should return as a network adapter"
                              : "CD-ROM mode: eject failed", a);
        dev->driver_data = a;
        return PROBE_BOUND;
    }

    // First words with the chip: a register read that must come back whole
    if (!rtw_read32(dev, REG_SYS_CFG, &a->sys_cfg) || a->sys_cfg == 0xFFFFFFFFU) {
        rtw_print_ids("did not answer a register read", a);
        free(a);
        return PROBE_FAILED;
    }
    if (family == WLAN_FAMILY_RTL8821C && !rtw_read8(dev, REG_SYS_CFG2, &a->chip_id))
        a->chip_id = 0;

    mutex_lock(&rtw_lock);
    int index = -1;
    for (int i = 0; i < RTW_MAX_ADAPTERS && index < 0; i++) {
        if (adapters[i] == NULL)
            index = i;
    }
    if (index >= 0) {
        adapters[index] = a;
        a->index = (uint8_t)index;
        a->state = WLAN_STATE_PROBED;
    }
    mutex_unlock(&rtw_lock);
    if (index < 0) {
        rtw_print_ids("ignored: too many adapters", a);
        free(a);
        return PROBE_FAILED;
    }

    a->wlan = wlan_register(&rtw_wlan_ops, a);
    if (a->wlan == NULL) {
        rtw_print_ids("ignored: no free wlanN name", a);
        mutex_lock(&rtw_lock);
        adapters[index] = NULL;
        mutex_unlock(&rtw_lock);
        free(a);
        return PROBE_FAILED;
    }

    // Same name in the IP stack; a missing interface is not fatal
    a->netif = netif_register(wlan_dev_name(a->wlan), &rtw_net_ops, a, NULL, NET_MTU_DEFAULT);

    rtw_print_ids(rtw_chip_name(a), a);
    if (speed == USB_SPEED_FULL)
        rtw_log("on a USB 1.1 port: 12 Mbit/s at most");

    // The efuse takes a few hundred register round trips: poll() does it
    mutex_lock(&rtw_lock);
    a->efuse_pending = true;
    mutex_unlock(&rtw_lock);

    dev->driver_data = a;
    return PROBE_BOUND;
}

static void rtw_remove(struct Device* dev) {
    struct RtwAdapter* a = (struct RtwAdapter*)dev->driver_data;
    if (a == NULL)
        return;
    if (a->family == RTW_ZEROCD) {
        free(a);
        return;
    }

    // Wait for poll() to let go of it; with the device unplugged its
    // transfers fail at once
    mutex_lock(&rtw_lock);
    a->removing = true;
    a->state = WLAN_STATE_GONE;
    while (a->busy) {
        mutex_unlock(&rtw_lock);
        thread_sleep(5);
        mutex_lock(&rtw_lock);
    }
    adapters[a->index] = NULL;
    // poll() let go of these for good (busy is clear and removing set). Take
    // them out under the lock: an ioctl still running until
    // wlan_unregister() below reads a->radio under it.
    struct UsbPipe*    rx_pipe   = a->rx_pipe;
    uint8_t*           rx_buffer = a->rx_buffer;
    struct R92eRadio*  radio     = a->radio;
    a->rx_pipe   = NULL;
    a->rx_buffer = NULL;
    a->radio     = NULL;
    a->radio_up  = false;
    mutex_unlock(&rtw_lock);

    usb_pipe_close(rx_pipe);
    free(rx_buffer);
    free(radio);

    // Waits out a transmit in progress (net lock)
    netif_unregister(a->netif);
    // Revokes open handles, and waits out a call in progress
    wlan_unregister(a->wlan);
    free(a);
}

//
// Radio and scanning (from poll(), with `busy` set and no lock held)
//

static bool rtw_bus_read(void* ctx, uint16_t reg, void* out, uint16_t length) {
    return rtw_read((const struct Device*)ctx, reg, out, length);
}

static bool rtw_bus_write(void* ctx, uint16_t reg, const void* data, uint16_t length) {
    return rtw_write((const struct Device*)ctx, reg, data, length);
}

static void rtw_bus_sleep(uint32_t ms) {
    thread_sleep(ms);
}

static uint64_t rtw_bus_now(void) {
    return timer_get_ms();
}

static bool rtw_radio_up(struct RtwAdapter* a) {
    if (a->radio == NULL) {
        a->radio = (struct R92eRadio*)malloc(sizeof(struct R92eRadio));
        if (a->radio == NULL)
            return false;
    }
    memset(a->radio, 0, sizeof(*a->radio));
    struct R92eRadio* r = a->radio;
    r->bus.ctx      = a->dev;
    r->bus.read     = rtw_bus_read;
    r->bus.write    = rtw_bus_write;
    r->bus.sleep_ms = rtw_bus_sleep;
    r->bus.now_ms   = rtw_bus_now;
    r->crystalcap   = a->crystalcap;
    if (a->mac_valid)
        memcpy(r->mac, a->mac, 6);

    if (a->rx_buffer == NULL)
        a->rx_buffer = (uint8_t*)malloc(USB_PIPE_MAX);
    if (a->rx_pipe == NULL) {
        const struct UsbInterface* itf = usb_dev_interface(a->dev);
        if (itf != NULL)
            a->rx_pipe = usb_pipe_open_bulk(a->dev, itf->bulk_in_endpoint, true);
    }
    if (a->rx_buffer == NULL || a->rx_pipe == NULL) {
        rtw_log("no receive pipe");
        return false;
    }

    uint32_t fw_length = 0;
    // <home>/sys/firmware/rtl8192eu_nic.bin, read by the kernel: its
    // contents only go to the chip
    uint8_t* fw = (uint8_t*)kapi->firmware_load(R92E_FW_NAME, R92E_FW_MAX, &fw_length);
    bool have_fw = fw != NULL;
    bool ok = r92e_radio_on(r, fw, fw_length);
    free(fw);

    if (ok) {
        rtw_log("%s: radio on (%s)", wlan_dev_name(a->wlan),
                r->fw_loaded ? "firmware loaded"
                             : (have_fw ? "firmware rejected, running without"
                                        : "no sys/firmware/" R92E_FW_NAME ", running without"));
    } else {
        rtw_log("%s: radio failed: %s", wlan_dev_name(a->wlan), r->error != NULL ? r->error : "?");
    }
    return ok;
}

struct RtwScanCtx {
    struct RtwAdapter* a;
    uint8_t channel;
};

// One received frame: keep it if it's a beacon or probe response
static void rtw_scan_frame(void* ctx, const struct WlanRxFrame* f) {
    struct RtwScanCtx* c = (struct RtwScanCtx*)ctx;
    struct WlanBss bss;
    if (!wlan_parse_beacon(f->data, f->length, c->channel, &bss))
        return;
    bss.rssi = f->rssi;
    mutex_lock(&rtw_lock);
    if (!c->a->removing && c->a->scan_state == WLAN_SCAN_RUNNING)
        wlan_scan_merge(c->a->scan, &c->a->scan_count, WLAN_SCAN_MAX, &bss);
    mutex_unlock(&rtw_lock);
}

// Listen on one channel for WLAN_SCAN_DWELL_MS. False if the adapter failed.
static bool rtw_scan_channel(struct RtwAdapter* a, uint8_t channel) {
    if (!r92e_set_channel(a->radio, channel))
        return false;

    struct RtwScanCtx ctx = { a, channel };
    uint64_t end = timer_get_ms() + WLAN_SCAN_DWELL_MS;
    bool skip_rest = false;     // A transfer filled the buffer: its frame runs on
    while (!a->removing) {
        uint64_t now = timer_get_ms();
        if (now >= end)
            break;
        uint32_t wait = (uint32_t)(end - now);
        int n = usb_pipe_transfer(a->rx_pipe, a->rx_buffer, USB_PIPE_MAX, wait < 10 ? 10 : wait);
        if (n == USB_ERR_TIMEOUT)
            continue;
        if (n == USB_ERR_STALL) {
            usb_pipe_clear_halt(a->rx_pipe);
            continue;
        }
        if (n < 0)
            return false;   // Gone, or the controller gave up on it
        // Frames are never split across transfers we keep: a full buffer
        // means one ran past it, and the next transfer is its tail
        if (!skip_rest)
            rtw_rx_walk(a->rx_buffer, (uint32_t)n, a->radio->cck_hipwr, rtw_scan_frame, &ctx);
        skip_rest = n == USB_PIPE_MAX;
    }
    return true;
}

static void rtw_poll(void) {
    if (!rtw_ready)
        return;

    // One adapter, one step per call: the efuse first, then scans
    struct RtwAdapter* a = NULL;
    bool efuse = false;
    mutex_lock(&rtw_lock);
    for (int i = 0; i < RTW_MAX_ADAPTERS && a == NULL; i++) {
        if (adapters[i] != NULL && adapters[i]->efuse_pending && !adapters[i]->removing) {
            a = adapters[i];
            efuse = true;
        }
    }
    for (int i = 0; i < RTW_MAX_ADAPTERS && a == NULL; i++) {
        if (adapters[i] != NULL && adapters[i]->scan_state == WLAN_SCAN_RUNNING && !adapters[i]->removing)
            a = adapters[i];
    }
    if (a != NULL) {
        a->efuse_pending = false;
        a->busy = true;
    }
    mutex_unlock(&rtw_lock);
    if (a == NULL)
        return;

    if (efuse) {
        uint8_t mac[6];
        uint8_t cap = a->crystalcap;
        bool ok = rtw_read_mac(a->dev, a->family, mac, &cap);
        // Still busy, so remove() can't free the interface under us; outside
        // rtw_lock, which never wraps another lock
        if (ok && a->netif != NULL)
            netif_set_mac(a->netif, mac);

        mutex_lock(&rtw_lock);
        if (!a->removing) {
            if (ok)
                memcpy(a->mac, mac, 6);
            a->mac_valid = ok;
            a->crystalcap = cap;
            a->state = ok ? WLAN_STATE_IDENTIFIED : WLAN_STATE_NO_EFUSE;
        }
        a->busy = false;
        mutex_unlock(&rtw_lock);
        return;
    }

    // Scanning: bring the radio up first (one call), then a channel per call
    bool ok;
    uint8_t channel = 0;
    if (!a->radio_up) {
        ok = rtw_radio_up(a);
        a->radio_up = ok;
    } else {
        mutex_lock(&rtw_lock);
        channel = a->scan_channel;
        mutex_unlock(&rtw_lock);
        ok = rtw_scan_channel(a, channel);
        if (!ok)
            a->radio_up = false;    // Bring it up again next time
    }

    mutex_lock(&rtw_lock);
    if (!a->removing) {
        if (!ok) {
            a->scan_state = WLAN_SCAN_FAILED;
            a->state = WLAN_STATE_RADIO_FAILED;
        } else {
            a->state = WLAN_STATE_RADIO_ON;
            if (channel != 0) {
                if (channel >= WLAN_CHANNEL_MAX)
                    a->scan_state = WLAN_SCAN_DONE;
                else
                    a->scan_channel = (uint8_t)(channel + 1);
            }
        }
    }
    a->busy = false;
    mutex_unlock(&rtw_lock);
}

static const struct Driver rtw_usb_driver = {
    .name    = "rtw-usb",
    .abi     = DRIVER_ABI,
    .flags   = DRIVER_HOTPLUG,
    .version = "0.4",
    .match   = rtw_match,
    .probe   = rtw_probe,
    .remove  = rtw_remove,
    .poll    = rtw_poll,
    .poll_ms = RTW_POLL_MS,
};

// Loaded by the kernel from <home>/sys/drivers/rtw-usb when one of its
// sticks is plugged in (driver/autoload.c), or by "load" in the console.
// The kernel then offers it the stick.
KDRV_ENTRY(api) {
    kapi = api;
    if (api->version < 5 || api->size < sizeof(struct KernelApi) || api->usb_control == NULL)
        return NULL;
    if (!rtw_ready) {
        rtw_lock = api->mutex_create();
        if (rtw_lock == NULL)
            return NULL;
        rtw_ready = true;
    }
    return &rtw_usb_driver;
}
