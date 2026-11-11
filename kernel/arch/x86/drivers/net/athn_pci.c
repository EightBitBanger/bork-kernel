#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/arch/x86/bus/pci/pci.h>
#include <kernel/arch/x86/drivers/net/ar9485.h>
#include <kernel/arch/x86/drivers/net/wlan.h>
#include <kernel/arch/x86/drivers/net/wlan_dev.h>
#include <kernel/arch/x86/drivers/net/wlan_scan.h>
#include <kernel/arch/x86/drivers/net/wlan_sta.h>
#include <kernel/arch/x86/io.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/driver/driver.h>
#include <kernel/driver/device.h>
#include <kernel/net/net.h>

#include <kernel/console/print.h>
#include <kernel/crypt/random.h>
#include <kernel/kernel.h>
#include <kernel/malloc.h>
#include <kernel/mutex.h>
#include <kernel/scheduler/process.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/syscall/syscall.h>
#include <kernel/util/snprint.h>
#include <kernel/util/string.h>
#include <kernel/util/timer.h>

//
// Atheros AR9485 PCIe Wi-Fi (the AR5B125 Mini PCIe card, 168c:0032)
//
// What it does: binds the card, maps its registers, resets it and reads
// its ROM for the MAC address, and offers /dev/wlanN (wlan_dev.c) and a
// network interface of the same name. A scan tunes channels 1-13 one at a
// time and listens for beacons (passive: nothing is transmitted). A
// connect joins one network, open or WPA2-Personal: wlan_sta.c does the
// joining and the encryption, this file gives it the card. Once the
// station says it is connected, the interface gets link and DHCP starts;
// Ethernet frames from the IP stack go out as 802.11 data frames, and data
// frames in come back as Ethernet frames. The card needs no firmware.
//
// The chip itself is ar9485.c. This file is the PCI side: registers, DMA
// pages, the scan, the station's hardware, and what goes in the kernel
// log. It polls; the card's interrupt stays off. While joining or joined,
// each poll serves the station for ATHN_STA_SLICE_MS, emptying the receive
// buffers every ATHN_HARVEST_MS.
//
// Device data and DMA are untrusted: register offsets are checked against
// the mapped window, the ROM and every receive and transmit status are
// checked in ar9485.c, and frames are copied out of DMA memory before
// parsing. Bus mastering is turned off before a DMA page is freed.
//
// What it does not do yet: rate control (data goes at up to 24 Mbit/s),
// 802.11n rates, power saving, 5 GHz, or other Atheros chips (each needs
// its own tables).
//

#define ATHN_MAX_ADAPTERS       2
#define ATHN_POLL_MS            50
#define ATHN_MMIO_SIZE          0x20000     // Every register used is below 0x17000
#define ATHN_HARVEST_MS         10          // How often a channel's buffers are emptied
#define ATHN_STA_HARVEST_MS     5           // ... while joining or joined
#define ATHN_STA_SLICE_MS       40          // How long one poll serves the station
#define ATHN_PAGE_COUNT         (AR9485_RX_COUNT + AR9485_TX_COUNT + 1)

static const struct DeviceMatch athn_match[] = {
    { .bus = BUS_PCI, .vendor = AR9485_VENDOR, .product = AR9485_DEVICE,
      .class_code = MATCH_ANY, .subclass = MATCH_ANY, .protocol = MATCH_ANY },
    { 0 }
};

struct AthnAdapter {
    struct Device*  dev;
    struct WlanDev* wlan;
    struct NetIf*   netif;              // NULL if the stack had no room
    uint8_t  index;                     // Slot in adapters[]
    uint8_t  pci_bus, pci_dev, pci_func;
    volatile uint8_t* regs;             // ATHN_MMIO_SIZE bytes of BAR 0
    uint8_t  state;                     // WLAN_STATE_*
    volatile uint32_t busy;             // poll() or a wlan op is using it without the lock
    bool     removing;
    bool     verbose;                   // The scan was started from the kernel console

    // Scanning. chip and the DMA pages belong to poll() while it scans
    // (only touched while busy, or by remove() once busy is clear).
    struct Ar9485 chip;
    uint8_t  eep[AR9485_EEP_IMAGE_SIZE];
    bool     pages_ok;
    uint8_t  scan_state;                // WLAN_SCAN_*
    uint8_t  scan_channel;              // Next channel to listen to (RUNNING)
    uint8_t  scan_count;
    struct WlanBss scan[WLAN_SCAN_MAX];

    // Joining and joined. The station, and the chip while sta_on, belong
    // to sta_lock. It is a bus lock: the IP stack's transmit takes it with
    // the net lock held, so nothing that takes the net lock is called with
    // it held (netif_set_link waits until it is let go).
    mutex_t  sta_lock;
    struct WlanSta sta;
    bool     sta_on;                    // Set and cleared under sta_lock and athn_lock: no scan meanwhile
    bool     link_want;                 // What the station says (sta_lock)
    bool     link_up;                   // What the IP stack was told (poll and remove only)
    uint32_t entropy[8];                // When frames arrived, for the station's nonces (sta_lock)
    uint8_t  entropy_next;
};

// adapters[] and the fields above, except while `busy`. Never held while
// taking another lock: the wlan ops take it with the VFS or wlan lock held.
static mutex_t athn_lock;
static bool athn_ready = false;
static struct AthnAdapter* adapters[ATHN_MAX_ADAPTERS];

//
// Kernel log. Everything starts "athn: " so the lines are easy to find and
// send back. At run time only scans started from the kernel console print:
// the desktop owns the screen otherwise.
//

static void athn_log(const char* text) {
    print("athn: ");
    print(text);
    print("\n");
}

static void athn_log_scan(const struct AthnAdapter* a, const char* text) {
    if (a->verbose)
        athn_log(text);
}

//
// Registers (no lock: MMIO). Offsets the window doesn't cover read as a
// card that went away, and writes to them go nowhere.
//

static uint32_t athn_reg_read(void* ctx, uint32_t reg) {
    struct AthnAdapter* a = (struct AthnAdapter*)ctx;
    if ((reg & 3) != 0 || reg > ATHN_MMIO_SIZE - 4)
        return 0xFFFFFFFFU;
    return *(volatile uint32_t*)(a->regs + reg);
}

static void athn_reg_write(void* ctx, uint32_t reg, uint32_t value) {
    struct AthnAdapter* a = (struct AthnAdapter*)ctx;
    if ((reg & 3) != 0 || reg > ATHN_MMIO_SIZE - 4)
        return;
    *(volatile uint32_t*)(a->regs + reg) = value;
}

// The host test (test/athn) puts a fake chip behind these
#ifndef ATHN_REG_READ
#define ATHN_REG_READ   athn_reg_read
#define ATHN_REG_WRITE  athn_reg_write
#endif

// Busy wait: the chip wants microseconds, the timer counts milliseconds.
// An I/O port write takes about a microsecond on any PC.
static void athn_delay_us(uint32_t us) {
    if (us >= 1000) {
        uint64_t start = timer_get_ms();
        uint32_t ms = (us + 999) / 1000;
        while (timer_get_ms() - start < ms)
            io_wait();
        return;
    }
    for (uint32_t i = 0; i < us; i++)
        io_wait();
}

//
// DMA pages: receive buffers, transmit buffers and the transmit status
// ring, one page each (physically contiguous by being one page)
//

static struct Ar9485Page* athn_page(struct AthnAdapter* a, uint32_t i) {
    if (i < AR9485_RX_COUNT)
        return &a->chip.rx[i];
    i -= AR9485_RX_COUNT;
    if (i < AR9485_TX_COUNT)
        return &a->chip.tx[i];
    return &a->chip.tx_status;
}

static void athn_free_pages(struct AthnAdapter* a) {
    for (uint32_t i = 0; i < ATHN_PAGE_COUNT; i++) {
        struct Ar9485Page* p = athn_page(a, i);
        if (p->virt != NULL)
            vmm_free_pages(p->virt, 1);
        p->virt = NULL;
        p->phys = 0;
    }
    a->pages_ok = false;
}

static bool athn_alloc_pages(struct AthnAdapter* a) {
    for (uint32_t i = 0; i < ATHN_PAGE_COUNT; i++) {
        struct Ar9485Page* p = athn_page(a, i);
        uint8_t* page = (uint8_t*)vmm_alloc_pages(1);
        if (page == NULL) {
            athn_free_pages(a);
            return false;
        }
        memset(page, 0, PAGE_SIZE);
        p->virt = page;
        p->phys = vmm_get_phys_addr(page);
        if (p->phys == 0) {
            athn_free_pages(a);
            return false;
        }
    }
    a->pages_ok = true;
    return true;
}

// A wlan op or poll() starts using the adapter without athn_lock: false
// once it is going away (remove() waits for busy to drop to 0)
static bool athn_hold(struct AthnAdapter* a) {
    mutex_lock(&athn_lock);
    bool ok = !a->removing;
    if (ok)
        a->busy++;
    mutex_unlock(&athn_lock);
    return ok;
}

static void athn_release(struct AthnAdapter* a) {
    mutex_lock(&athn_lock);
    a->busy--;
    mutex_unlock(&athn_lock);
}

//
// /dev/wlanN (wlan_dev.c routes it here)
//

static int32_t athn_wlan_info(void* ctx, struct WlanInfo* out) {
    struct AthnAdapter* a = (struct AthnAdapter*)ctx;
    mutex_lock(&athn_lock);
    bool gone = a->removing;
    if (!gone) {
        out->version   = WLAN_INFO_VERSION;
        out->vendor    = a->dev->vendor;
        out->product   = a->dev->product;
        out->family    = WLAN_FAMILY_AR9485;
        out->state     = a->state;
        out->usb_speed = 0;
        out->mac_valid = a->chip.mac_valid ? 1 : 0;
        if (a->chip.mac_valid)
            memcpy(out->mac, a->chip.mac, 6);
        out->chip_cut  = a->chip.mac_rev;
        out->chip_id   = 0;
        out->sys_cfg   = a->chip.srev;
        strncpy(out->chip, "AR9485", sizeof(out->chip) - 1);
    }
    mutex_unlock(&athn_lock);
    return gone ? SYS_ENOENT : SYS_OK;
}

static int32_t athn_wlan_scan_start(void* ctx) {
    struct AthnAdapter* a = (struct AthnAdapter*)ctx;
    mutex_lock(&athn_lock);
    int32_t result = SYS_OK;
    if (a->removing)
        result = SYS_ENOENT;
    else if (a->state == WLAN_STATE_PROBED)
        result = SYS_EAGAIN;
    else if (!a->pages_ok)
        result = SYS_EIO;
    else if (a->scan_state == WLAN_SCAN_RUNNING || a->sta_on)
        result = SYS_EBUSY;             // Joining or joined: the radio stays on its channel
    if (result == SYS_OK) {
        a->scan_state   = WLAN_SCAN_RUNNING;
        a->scan_channel = 1;
        a->scan_count   = 0;
        memset(a->scan, 0, sizeof(a->scan));
        // Kernel console (no process): say what happens as it happens
        a->verbose = process_current_id() == PROCESS_INVALID_ID;
    }
    mutex_unlock(&athn_lock);
    return result;
}

static int32_t athn_wlan_scan_results(void* ctx, struct WlanScanResults* out) {
    struct AthnAdapter* a = (struct AthnAdapter*)ctx;
    mutex_lock(&athn_lock);
    bool gone = a->removing;
    if (!gone) {
        out->version  = WLAN_SCAN_VERSION;
        out->state    = a->scan_state;
        out->channel  = a->scan_state == WLAN_SCAN_RUNNING ? a->scan_channel : 0;
        out->count    = a->scan_count <= WLAN_SCAN_MAX ? a->scan_count : WLAN_SCAN_MAX;
        out->firmware = 1;                  // Needs none
        memcpy(out->bss, a->scan, sizeof(struct WlanBss) * out->count);
    }
    mutex_unlock(&athn_lock);
    return gone ? SYS_ENOENT : SYS_OK;
}

//
// The station's hardware (wlan_sta.h): called with sta_lock held
//

// The cycle counter and, where the CPU has it, RDRAND: stirred into the
// random pool with the frame arrival times before every nonce
static uint64_t athn_cycles(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static bool athn_rdrand(uint32_t* out) {
    uint32_t eax = 1, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if (!(ecx & (1U << 30)))
        return false;
    for (int i = 0; i < 10; i++) {
        uint8_t ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(*out), "=qm"(ok));
        if (ok)
            return true;
    }
    return false;
}

static void athn_sta_random(void* ctx, uint8_t* out, uint32_t length) {
    struct AthnAdapter* a = (struct AthnAdapter*)ctx;
    uint32_t mix[16];
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t r = 0;
        (void)athn_rdrand(&r);
        mix[i] = r;
    }
    uint64_t t = athn_cycles();
    mix[8] = (uint32_t)t;
    mix[9] = (uint32_t)(t >> 32);
    mix[10] = (uint32_t)timer_get_ms();
    mix[11] = a->chip.rx_frames;
    mix[12] = a->chip.tx_frames;
    mix[13] = a->chip.rx_errors;
    mix[14] = (uint32_t)(uintptr_t)out;
    mix[15] = length;
    crypt_random_add(a->entropy, sizeof(a->entropy));
    crypt_random_add(mix, sizeof(mix));
    crypt_random_bytes(out, length);
}

// Rates in 500 kbit/s units (as in the beacon) to the card's codes
static uint8_t athn_rate_code(uint8_t rate) {
    switch (rate & 0x7f) {
        case 2:   return AR9485_RATE_CCK1;
        case 4:   return AR9485_RATE_CCK2;
        case 11:  return AR9485_RATE_CCK5;
        case 22:  return AR9485_RATE_CCK11;
        case 12:  return AR9485_RATE_OFDM6;
        case 18:  return AR9485_RATE_OFDM9;
        case 24:  return AR9485_RATE_OFDM12;
        case 36:  return AR9485_RATE_OFDM18;
        case 48:  return AR9485_RATE_OFDM24;
        case 72:  return AR9485_RATE_OFDM36;
        case 96:  return AR9485_RATE_OFDM48;
        case 108: return AR9485_RATE_OFDM54;
    }
    return 0;
}

static bool athn_has_rate(const struct WlanSta* s, uint8_t rate) {
    for (uint32_t i = 0; i < s->rate_count; i++) {
        if ((s->rates[i] & 0x7f) == rate)
            return true;
    }
    return false;
}

// The rates for a frame. Management, EAPOL (the handshake) and group
// frames go at the network's lowest basic rate, which every station there
// hears. Data tries up to three of 24, 12, 6 Mbit/s (11, 5.5, 2 on an
// 802.11b network) that the access point offers, then the basic rate.
// Nothing faster: without rate control (and without the TX I/Q calibration
// athn does) the fast rates would only add retries.
static void athn_pick_rates(const struct WlanSta* s, const uint8_t* frame, uint32_t length,
                            struct Ar9485TxRates* out) {
    uint8_t basic = 0, basic_value = 0xff;
    for (uint32_t i = 0; i < s->rate_count; i++) {
        uint8_t v = s->rates[i] & 0x7f;
        if ((s->rates[i] & 0x80) && athn_rate_code(v) != 0 && v < basic_value) {
            basic_value = v;
            basic = athn_rate_code(v);
        }
    }
    if (basic == 0)
        basic = AR9485_RATE_CCK1;
    memset(out, 0, sizeof(*out));
    out->rate[0] = basic;
    out->tries[0] = 8;

    static const uint8_t eapol_llc[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E };
    bool data = (frame[0] & 0x0c) == 0x08;
    bool group = (frame[4] & 0x01) != 0;
    bool eapol = data && !(frame[1] & 0x40) && length >= 24 + 8 && memcmp(frame + 24, eapol_llc, 8) == 0;
    if (!data || group || eapol)
        return;

    static const uint8_t ofdm[3] = { 48, 24, 12 }, cck[3] = { 22, 11, 4 };
    bool g = athn_has_rate(s, 48) || athn_has_rate(s, 24) || athn_has_rate(s, 12);
    const uint8_t* want = g ? ofdm : cck;
    uint32_t n = 0;
    for (uint32_t i = 0; i < 3; i++) {
        if (athn_has_rate(s, want[i]) && athn_rate_code(want[i]) != basic) {
            out->rate[n] = athn_rate_code(want[i]);
            out->tries[n] = 2;
            n++;
        }
    }
    out->rate[n] = basic;
    out->tries[n] = 4;
}

static int32_t athn_sta_transmit(void* ctx, const uint8_t* frame, uint32_t length) {
    struct AthnAdapter* a = (struct AthnAdapter*)ctx;
    if (length < 24)
        return SYS_EINVAL;
    struct Ar9485TxRates rates;
    athn_pick_rates(&a->sta, frame, length, &rates);
    if (a->chip.tx_queued >= AR9485_TX_COUNT)
        ar9485_tx_reclaim(&a->chip);
    return ar9485_tx(&a->chip, frame, length, &rates) ? SYS_OK : SYS_EIO;
}

static int32_t athn_sta_tune(void* ctx, uint8_t channel, const uint8_t* bssid) {
    struct AthnAdapter* a = (struct AthnAdapter*)ctx;
    if (bssid == NULL) {
        ar9485_stop(&a->chip);
        return SYS_OK;
    }
    if (!ar9485_tune(&a->chip, channel))
        return SYS_EIO;
    ar9485_set_bss(&a->chip, bssid);
    return SYS_OK;
}

static void athn_sta_deliver(void* ctx, const uint8_t* frame, uint32_t length) {
    struct AthnAdapter* a = (struct AthnAdapter*)ctx;
    if (a->netif != NULL)
        netif_rx(a->netif, frame, length);      // Only queues: fine under sta_lock
}

static void athn_sta_link(void* ctx, bool up) {
    ((struct AthnAdapter*)ctx)->link_want = up;  // poll() tells the IP stack, without sta_lock
}

static const struct WlanStaOps athn_sta_ops = {
    .transmit = athn_sta_transmit,
    .tune     = athn_sta_tune,
    .deliver  = athn_sta_deliver,
    .link     = athn_sta_link,
    .random   = athn_sta_random,
};

// One frame received while joining or joined
static void athn_sta_frame(void* ctx, const struct WlanRxFrame* f) {
    struct AthnAdapter* a = (struct AthnAdapter*)ctx;
    a->entropy[a->entropy_next++ % 8] ^= (uint32_t)athn_cycles() ^ f->length ^ ((uint32_t)(uint8_t)f->rssi << 24);
    wlan_sta_rx(&a->sta, f->data, f->length, f->rssi, timer_get_ms());
}

// sta_on, with both locks (sta_lock held by the caller)
static void athn_set_sta_on(struct AthnAdapter* a, bool on) {
    mutex_lock(&athn_lock);
    a->sta_on = on;
    if (on)
        a->state = WLAN_STATE_RADIO_ON;
    mutex_unlock(&athn_lock);
}

// Tell the IP stack about link changes (no driver lock held)
static void athn_update_link(struct AthnAdapter* a) {
    mutex_lock(&a->sta_lock);
    bool want = a->link_want;
    mutex_unlock(&a->sta_lock);
    if (want != a->link_up && a->netif != NULL) {
        a->link_up = want;
        netif_set_link(a->netif, want);
    }
}

//
// Connecting (wlan_dev.c routes WLAN_IOCTL_CONNECT / DISCONNECT / LINK here)
//

static int32_t athn_wlan_connect(void* ctx, const struct WlanBss* bss, const uint8_t* pmk) {
    struct AthnAdapter* a = (struct AthnAdapter*)ctx;
    if (!athn_hold(a))
        return SYS_ENOENT;
    mutex_lock(&athn_lock);
    int32_t result = SYS_OK;
    if (a->state == WLAN_STATE_PROBED)
        result = SYS_EAGAIN;
    else if (!a->pages_ok || !a->chip.mac_valid)
        result = SYS_EIO;               // Nothing to send from, or no address to send as
    else if (a->scan_state == WLAN_SCAN_RUNNING)
        result = SYS_EBUSY;
    mutex_unlock(&athn_lock);

    if (result == SYS_OK) {
        mutex_lock(&a->sta_lock);
        // Off the scan's hands now (it isn't running and can't start while
        // sta_on), so the station may tune it
        athn_set_sta_on(a, true);
        result = wlan_sta_connect(&a->sta, bss, pmk, timer_get_ms());
        if (a->sta.state == WLAN_LINK_IDLE) {
            ar9485_stop(&a->chip);
            athn_set_sta_on(a, false);
            if (a->chip.switched_off) {
                mutex_lock(&athn_lock);
                a->state = WLAN_STATE_RADIO_OFF;
                mutex_unlock(&athn_lock);
            }
        }
        mutex_unlock(&a->sta_lock);
        athn_update_link(a);            // A connection it replaced had link
    }
    athn_release(a);
    return result;
}

static int32_t athn_wlan_disconnect(void* ctx) {
    struct AthnAdapter* a = (struct AthnAdapter*)ctx;
    if (!athn_hold(a))
        return SYS_ENOENT;
    mutex_lock(&a->sta_lock);
    if (a->sta_on) {
        wlan_sta_disconnect(&a->sta, WLAN_REASON_USER);    // Says goodbye, tunes away
        ar9485_stop(&a->chip);
        athn_set_sta_on(a, false);
    }
    mutex_unlock(&a->sta_lock);
    athn_update_link(a);
    athn_release(a);
    return SYS_OK;
}

static int32_t athn_wlan_link(void* ctx, struct WlanLink* out) {
    struct AthnAdapter* a = (struct AthnAdapter*)ctx;
    if (!athn_hold(a))
        return SYS_ENOENT;
    mutex_lock(&a->sta_lock);
    wlan_sta_link(&a->sta, out);
    bool connected = a->sta.state == WLAN_LINK_CONNECTED;
    mutex_unlock(&a->sta_lock);
    struct NetIf* ifp = a->netif;
    if (connected && ifp != NULL) {
        // Single words the net thread writes: a torn read is impossible
        out->ip      = ifp->ip;
        out->gateway = ifp->gateway;
    }
    athn_release(a);
    return SYS_OK;
}

// Serve the station for one slice: frames in, transmit statuses back,
// timeouts (from poll(), held)
static void athn_sta_serve(struct AthnAdapter* a) {
    uint64_t end = timer_get_ms() + ATHN_STA_SLICE_MS;
    while (1) {
        mutex_lock(&a->sta_lock);
        if (a->sta_on) {
            ar9485_rx_harvest(&a->chip, athn_sta_frame, a);
            ar9485_tx_reclaim(&a->chip);
            wlan_sta_tick(&a->sta, timer_get_ms());
            if (a->sta.state == WLAN_LINK_IDLE) {
                // Gave up or was sent away: the station has tuned away
                ar9485_stop(&a->chip);
                athn_set_sta_on(a, false);
            }
        }
        bool on = a->sta_on;
        mutex_unlock(&a->sta_lock);
        athn_update_link(a);
        if (!on || a->removing || timer_get_ms() >= end)
            break;
        thread_sleep(ATHN_STA_HARVEST_MS);
    }
}

static const struct WlanOps athn_wlan_ops = {
    .info         = athn_wlan_info,
    .scan_start   = athn_wlan_scan_start,
    .scan_results = athn_wlan_scan_results,
    .connect      = athn_wlan_connect,
    .disconnect   = athn_wlan_disconnect,
    .link         = athn_wlan_link,
};

// Network interface: Ethernet frames out as 802.11 data frames, once the
// station is connected (net lock held; takes sta_lock)
static int athn_net_transmit(struct NetIf* ifp, const void* frame, uint32_t length) {
    struct AthnAdapter* a = ifp != NULL ? (struct AthnAdapter*)ifp->driver_data : NULL;
    if (a == NULL)
        return NET_ERR_DOWN;
    mutex_lock(&a->sta_lock);
    int32_t result = a->sta_on ? wlan_sta_send(&a->sta, (const uint8_t*)frame, length) : SYS_EAGAIN;
    mutex_unlock(&a->sta_lock);
    if (result == SYS_OK)
        return NET_OK;
    return result == SYS_EAGAIN ? NET_ERR_DOWN : NET_ERR_IO;
}

static const struct NetIfOps athn_net_ops = {
    .transmit = athn_net_transmit,
};

//
// Binding
//

// One boot line: manufacturer, chip, revision ("Atheros AR9485 rev 1")
static void athn_log_identity(const struct AthnAdapter* a) {
    char line[48];
    snprintf(line, sizeof(line), "Atheros AR9485 rev %u\n", (unsigned)a->chip.mac_rev);
    print(line);
}

// Stop the card touching memory: receive off, chip asleep, no bus master
static void athn_quiesce(struct AthnAdapter* a) {
    if (a->regs != NULL && a->chip.bus.read != NULL)
        ar9485_stop(&a->chip);
    uint16_t cmd = pci_read_command(a->pci_bus, a->pci_dev, a->pci_func);
    pci_write_command(a->pci_bus, a->pci_dev, a->pci_func, (uint16_t)(cmd & ~PCI_COMMAND_BUS_MASTER));
}

static int athn_probe(struct Device* dev) {
    if (!athn_ready) {
        mutex_init(&athn_lock);
        athn_ready = true;
    }
    const struct PCIDeviceInfo* pci = (const struct PCIDeviceInfo*)dev->bus_data;
    if (dev->bus != BUS_PCI || pci == NULL)
        return PROBE_NOT_MINE;

    struct AthnAdapter* a = (struct AthnAdapter*)malloc(sizeof(struct AthnAdapter));
    if (a == NULL)
        return PROBE_FAILED;
    memset(a, 0, sizeof(*a));
    a->dev      = dev;
    a->pci_bus  = pci->bus;
    a->pci_dev  = pci->dev;
    a->pci_func = pci->func;

    // Memory decoding on, its interrupt line off (polled). Bus mastering
    // waits until there are receive buffers for the card to write to.
    pci_enable_command_bits(a->pci_bus, a->pci_dev, a->pci_func,
                            PCI_COMMAND_MEMORY | PCI_COMMAND_INTX_DISABLE);
    athn_quiesce(a);
    a->regs = (volatile uint8_t*)pci_map_bar(a->pci_bus, a->pci_dev, a->pci_func, 0, ATHN_MMIO_SIZE,
                                             VM_PRESENT | VM_READWRITE | VM_PCD);
    if (a->regs == NULL) {
        athn_log("168c:0032: BAR 0 could not be mapped (unassigned, or above 4 GB)");
        athn_quiesce(a);
        free(a);
        return PROBE_FAILED;
    }
    a->chip.bus.ctx      = a;
    a->chip.bus.read     = ATHN_REG_READ;
    a->chip.bus.write    = ATHN_REG_WRITE;
    a->chip.bus.delay_us = athn_delay_us;
    mutex_init(&a->sta_lock);

    char line[112];
    if (!ar9485_identify(&a->chip)) {
        snprintf(line, sizeof(line), "168c:0032: %s (AR_SREV 0x%08x), left alone",
                 a->chip.error != NULL ? a->chip.error : "?", (unsigned)a->chip.srev);
        athn_log(line);
        athn_quiesce(a);
        free(a);
        return PROBE_FAILED;
    }

    mutex_lock(&athn_lock);
    int index = -1;
    for (int i = 0; i < ATHN_MAX_ADAPTERS && index < 0; i++) {
        if (adapters[i] == NULL)
            index = i;
    }
    if (index >= 0) {
        adapters[index] = a;
        a->index = (uint8_t)index;
        a->state = WLAN_STATE_PROBED;
    }
    mutex_unlock(&athn_lock);
    if (index < 0) {
        athn_log("168c:0032: ignored: too many adapters");
        athn_quiesce(a);
        free(a);
        return PROBE_FAILED;
    }

    a->wlan = wlan_register(&athn_wlan_ops, a);
    if (a->wlan == NULL) {
        athn_log("168c:0032: ignored: no free wlanN name");
        mutex_lock(&athn_lock);
        adapters[index] = NULL;
        mutex_unlock(&athn_lock);
        athn_quiesce(a);
        free(a);
        return PROBE_FAILED;
    }

    // Reset and ROM: a few thousand register reads, done here so the
    // result is in the boot log
    bool attached = ar9485_attach(&a->chip, a->eep);
    bool pages = attached && athn_alloc_pages(a);
    if (pages)
        pci_enable_command_bits(a->pci_bus, a->pci_dev, a->pci_func, PCI_COMMAND_BUS_MASTER);
    if (!attached) {
        snprintf(line, sizeof(line), "%s: AR9485 did not come up: %s", wlan_dev_name(a->wlan),
                 a->chip.error != NULL ? a->chip.error : "?");
        athn_log(line);
    } else {
        athn_log_identity(a);
        if (!pages) {
            snprintf(line, sizeof(line), "%s: no memory for receive buffers: scanning off", wlan_dev_name(a->wlan));
            athn_log(line);
        }
    }

    wlan_sta_init(&a->sta, &athn_sta_ops, a, a->chip.mac);

    // Same name in the IP stack; a missing interface is not fatal
    a->netif = netif_register(wlan_dev_name(a->wlan), &athn_net_ops, a,
                              a->chip.mac_valid ? a->chip.mac : NULL, NET_MTU_DEFAULT);

    mutex_lock(&athn_lock);
    if (!attached)
        a->state = WLAN_STATE_RADIO_FAILED;
    else
        a->state = a->chip.mac_valid ? WLAN_STATE_IDENTIFIED : WLAN_STATE_NO_EFUSE;
    mutex_unlock(&athn_lock);

    dev->driver_data = a;
    return PROBE_BOUND;
}

static void athn_remove(struct Device* dev) {
    struct AthnAdapter* a = (struct AthnAdapter*)dev->driver_data;
    if (a == NULL)
        return;

    // Wait for poll() and the wlan ops to let go of it
    mutex_lock(&athn_lock);
    a->removing = true;
    a->state = WLAN_STATE_GONE;
    while (a->busy) {
        mutex_unlock(&athn_lock);
        thread_sleep(5);
        mutex_lock(&athn_lock);
    }
    adapters[a->index] = NULL;
    mutex_unlock(&athn_lock);

    // Leave the network (a transmit in progress finishes first), so the IP
    // stack's transmit stops reaching the card
    mutex_lock(&a->sta_lock);
    if (a->sta_on) {
        wlan_sta_disconnect(&a->sta, WLAN_REASON_RADIO);
        athn_set_sta_on(a, false);
    }
    a->link_want = false;
    mutex_unlock(&a->sta_lock);
    athn_update_link(a);

    // The card stops reading and writing before its pages go
    athn_quiesce(a);
    athn_free_pages(a);

    // Waits out a transmit in progress (net lock)
    netif_unregister(a->netif);
    // Revokes open handles, and waits out a call in progress
    wlan_unregister(a->wlan);
    free(a);
}

//
// Scanning (from poll(), with `busy` set and no lock held)
//

struct AthnScanCtx {
    struct AthnAdapter* a;
    uint8_t channel;
    uint32_t beacons;
};

// One received frame: keep it if it's a beacon or probe response
static void athn_scan_frame(void* ctx, const struct WlanRxFrame* f) {
    struct AthnScanCtx* c = (struct AthnScanCtx*)ctx;
    struct WlanBss bss;
    if (!wlan_parse_beacon(f->data, f->length, c->channel, &bss))
        return;
    bss.rssi = f->rssi;
    c->beacons++;
    mutex_lock(&athn_lock);
    if (!c->a->removing && c->a->scan_state == WLAN_SCAN_RUNNING)
        wlan_scan_merge(c->a->scan, &c->a->scan_count, WLAN_SCAN_MAX, &bss);
    mutex_unlock(&athn_lock);
}

// Tune `channel` and listen for WLAN_SCAN_DWELL_MS. False if the chip
// would not tune.
static bool athn_scan_channel(struct AthnAdapter* a, uint8_t channel) {
    char line[112];
    uint32_t frames_before = a->chip.rx_frames;
    uint32_t errors_before = a->chip.rx_errors;
    uint32_t bad_before    = a->chip.rx_bad;

    if (!ar9485_tune(&a->chip, channel)) {
        snprintf(line, sizeof(line), "%s: channel %u: %s", wlan_dev_name(a->wlan), (unsigned)channel,
                 a->chip.error != NULL ? a->chip.error : "?");
        athn_log_scan(a, line);
        return false;
    }

    struct AthnScanCtx ctx = { a, channel, 0 };
    uint64_t end = timer_get_ms() + WLAN_SCAN_DWELL_MS;
    while (!a->removing && timer_get_ms() < end) {
        thread_sleep(ATHN_HARVEST_MS);
        ar9485_rx_harvest(&a->chip, athn_scan_frame, &ctx);
    }
    ar9485_rx_harvest(&a->chip, athn_scan_frame, &ctx);

    snprintf(line, sizeof(line), "%s: channel %u: %u frames, %u beacons, %u CRC/PHY errors, %u bad",
             wlan_dev_name(a->wlan), (unsigned)channel, (unsigned)(a->chip.rx_frames - frames_before),
             (unsigned)ctx.beacons, (unsigned)(a->chip.rx_errors - errors_before),
             (unsigned)(a->chip.rx_bad - bad_before));
    athn_log_scan(a, line);
    return true;
}

static void athn_poll(void) {
    if (!athn_ready)
        return;

    // Every adapter joining or joined: a slice each
    for (int i = 0; i < ATHN_MAX_ADAPTERS; i++) {
        struct AthnAdapter* s = NULL;
        mutex_lock(&athn_lock);
        if (adapters[i] != NULL && adapters[i]->sta_on && !adapters[i]->removing) {
            s = adapters[i];
            s->busy++;
        }
        mutex_unlock(&athn_lock);
        if (s != NULL) {
            athn_sta_serve(s);
            athn_release(s);
        }
    }

    // One adapter, one channel per call
    struct AthnAdapter* a = NULL;
    uint8_t channel = 0;
    mutex_lock(&athn_lock);
    for (int i = 0; i < ATHN_MAX_ADAPTERS && a == NULL; i++) {
        if (adapters[i] != NULL && adapters[i]->scan_state == WLAN_SCAN_RUNNING && !adapters[i]->removing)
            a = adapters[i];
    }
    if (a != NULL) {
        a->busy++;
        channel = a->scan_channel;
    }
    mutex_unlock(&athn_lock);
    if (a == NULL)
        return;

    bool ok = athn_scan_channel(a, channel);
    bool last = !ok || channel >= WLAN_CHANNEL_MAX;
    if (last)
        ar9485_stop(&a->chip);       // Receive off and asleep until the next scan

    char line[112];
    if (!ok) {
        snprintf(line, sizeof(line), "%s: scan stopped: %s", wlan_dev_name(a->wlan),
                 a->chip.switched_off ? "the wireless switch has the radio off" : "the radio would not tune");
        athn_log_scan(a, line);
    }

    mutex_lock(&athn_lock);
    if (!a->removing) {
        if (!ok) {
            a->scan_state = WLAN_SCAN_FAILED;
            a->state = a->chip.switched_off ? WLAN_STATE_RADIO_OFF : WLAN_STATE_RADIO_FAILED;
        } else {
            a->state = WLAN_STATE_RADIO_ON;
            if (last)
                a->scan_state = WLAN_SCAN_DONE;
            else
                a->scan_channel = (uint8_t)(channel + 1);
        }
    }
    uint8_t found = a->scan_count;
    a->busy--;
    mutex_unlock(&athn_lock);

    if (ok && last) {
        snprintf(line, sizeof(line), "%s: scan done: %u network%s", wlan_dev_name(a->wlan),
                 (unsigned)found, found == 1 ? "" : "s");
        athn_log_scan(a, line);
    }
}

static const struct Driver athn_pci_driver = {
    .name    = "athn-pci",
    .abi     = DRIVER_ABI,
    .flags   = DRIVER_BUILTIN,
    .version = "0.1",
    .match   = athn_match,
    .probe   = athn_probe,
    .remove  = athn_remove,
    .poll    = athn_poll,
    .poll_ms = ATHN_POLL_MS,
};
DRIVER_BUILTIN_DECLARE(athn_pci_driver);
