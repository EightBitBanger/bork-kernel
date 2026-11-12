// Host test for the Atheros AR9485 driver: ar9485.c (reset, ROM, tuning,
// receive and transmit DMA, transmit power) against a fake chip, and
// athn_pci.c + wlan_dev.c + wlan_sta.c through the real driver/device.c,
// joining a simulated access point over the fake chip's air: open and
// WPA2 networks, data both ways, a wrong password, the access point going
// away, removal while connected. ASan + UBSan.
//
//   make            (builds and runs it)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

// The fake chip sits behind the driver's register accessors
static uint32_t fake_read(void* ctx, uint32_t reg);
static void fake_write(void* ctx, uint32_t reg, uint32_t value);
#define ATHN_REG_READ   fake_read
#define ATHN_REG_WRITE  fake_write

// No port I/O on the host: io_wait() is the fake chip's clock
#define _X86_IO_H_
static void fake_tick(void);
static inline void io_wait(void) { fake_tick(); }

#include "kernel/arch/x86/drivers/net/ar9485.c"
#include "kernel/arch/x86/drivers/net/wlan_scan.c"
#include "kernel/arch/x86/drivers/net/wlan_dev.c"
#include "kernel/arch/x86/drivers/net/athn_pci.c"
#include "kernel/driver/device.c"

// Built separately (their static helpers would clash with ar9485.c's):
// wlan_sta.c and kernel/crypt. The access point below uses kernel/crypt
// for its keys; tests/crypt checks those against the standards' vectors.
#include "kernel/crypt/aes.h"
#include "kernel/crypt/ccmp.h"
#include "kernel/crypt/wpa.h"

#define AR_EEP_COMPRESS_PAIRS   2       // Not handled by the driver
#define AR_RXS11_CRC_ERR        0x00000004

void* aligned_alloc(size_t alignment, size_t size);

static long checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

//
// The fake AR9485
//

#define FAKE_SREV       0x002401ffU     // version 0x240 (AR9485), revision 1, as ath9k decodes it
#define FAKE_FIFO       256

static struct {
    uint32_t regs[ATHN_MMIO_SIZE / 4];
    uint32_t srev;
    // ROM: a byte image, read as LE 16-bit words (EEPROM) or LE 32-bit words (OTP)
    uint8_t  rom[4096];
    int      rom_kind;                  // 0 none, 1 EEPROM, 2 OTP
    uint32_t rom_size;
    uint32_t eep_latch, otp_latch;
    bool     agc_stuck;
    bool     switch_off;                // The wireless switch GPIO reads 0
    // Receive FIFOs: the addresses the driver gave, in order
    uint32_t lp[FAKE_FIFO], hp[FAKE_FIFO];
    uint32_t lp_head, lp_tail, hp_head, hp_tail;
    bool     rx_dma;                    // AR_CR: receive enabled
    uint8_t  channel;
    // Failure injection: after this many accesses the card reads all ones
    long     die_after;
    long     accesses;
    // Transmitting: descriptor addresses written to AR_QTXDP(0), in order,
    // and the next status slot in the ring
    uint32_t txq[FAKE_FIFO];
    uint32_t tx_head, tx_tail;
    uint32_t status_slot;
    bool     tx_stall;                  // The card never sends (no statuses)
    bool     tx_deaf;                   // Nobody acknowledges
} fake;

// What the fake card sent, for the checks
struct Sent { uint8_t frame[2400]; uint32_t len; uint32_t ctl[32]; bool acked; };
static struct Sent sent[64];
static int sent_count = 0;              // All time (sent[] keeps the last 64)

// Physical addresses of receive pages, for the DMA the fake does
#define MAX_PAGES 128
static uint8_t* pages_virt[MAX_PAGES];
static uint32_t pages_phys[MAX_PAGES];
static int pages_live = 0;

static uint8_t* phys_to_virt(uint32_t phys) {
    for (int i = 0; i < MAX_PAGES; i++)
        if (pages_virt[i] != NULL && pages_phys[i] == phys) return pages_virt[i];
    return NULL;
}

static bool fake_dead(void) {
    return fake.die_after >= 0 && fake.accesses > fake.die_after;
}

static uint32_t fake_read(void* ctx, uint32_t reg) {
    (void)ctx;
    fake.accesses++;
    if (fake_dead()) return 0xFFFFFFFFU;
    CHECK((reg & 3) == 0 && reg < ATHN_MMIO_SIZE);
    switch (reg) {
        case AR_SREV:           return fake.srev;
        case AR_RTC_STATUS:     return AR_RTC_STATUS_ON;
        case AR_EEPROM_STATUS_DATA: {
            if (fake.rom_kind != 1) return AR_EEPROM_STATUS_DATA_BUSY;      // No EEPROM: never ready
            uint32_t a = fake.eep_latch * 2;
            if (a + 1 >= fake.rom_size) return 0xFFFF;
            return (uint32_t)fake.rom[a] | (uint32_t)fake.rom[a + 1] << 8;
        }
        case AR_OTP_STATUS:     return fake.rom_kind == 2 ? AR_OTP_STATUS_VALID : 0;
        case AR_OTP_READ_DATA: {
            uint32_t a = fake.otp_latch * 4;
            if (a + 3 >= fake.rom_size) return 0;
            return (uint32_t)fake.rom[a] | (uint32_t)fake.rom[a + 1] << 8 |
                   (uint32_t)fake.rom[a + 2] << 16 | (uint32_t)fake.rom[a + 3] << 24;
        }
        case AR_PHY_AGC_CONTROL:
            // Calibration finishes at once, unless stuck
            if (!fake.agc_stuck) fake.regs[reg / 4] &= ~AR_PHY_AGC_CONTROL_CAL;
            return fake.regs[reg / 4];
        case AR_CR:             return fake.rx_dma ? AR_CR_RXE : 0;
        case AR_GPIO_IN:        return fake.switch_off ? 0 : AR9300_GPIO_IN_VAL;
    }
    if (reg >= 0x2000 && reg < 0x2000 + 4 * 2048) { fake.eep_latch = (reg - 0x2000) / 4; return 0; }
    if (reg >= 0x14000 && reg < 0x14000 + 4 * 1024) { fake.otp_latch = (reg - 0x14000) / 4; return 0; }
    return fake.regs[reg / 4];
}

static void fake_write(void* ctx, uint32_t reg, uint32_t value) {
    (void)ctx;
    fake.accesses++;
    if (fake_dead()) return;
    CHECK((reg & 3) == 0 && reg < ATHN_MMIO_SIZE);
    fake.regs[reg / 4] = value;
    switch (reg) {
        case AR_LP_RXDP:
            CHECK(fake.lp_tail - fake.lp_head < FAKE_FIFO);
            CHECK(phys_to_virt(value) != NULL);         // Only ever pages it was given
            fake.lp[fake.lp_tail++ % FAKE_FIFO] = value;
            break;
        case AR_HP_RXDP:
            CHECK(fake.hp_tail - fake.hp_head < FAKE_FIFO);
            CHECK(phys_to_virt(value) != NULL);
            fake.hp[fake.hp_tail++ % FAKE_FIFO] = value;
            break;
        case AR_CR:
            if (value & AR_CR_RXD) {
                fake.rx_dma = false;
            } else if (value == 0) {
                fake.rx_dma = true;
            }
            break;
        case AR_RTC_RC:
            if (value != 0) {
                // A MAC reset empties the receive and transmit FIFOs
                fake.lp_head = fake.lp_tail; fake.hp_head = fake.hp_tail;
                fake.rx_dma = false;
                fake.tx_head = fake.tx_tail;
            }
            break;
        case AR_QTXDP(0):
            CHECK(fake.tx_tail - fake.tx_head < 8);                // The queue's FIFO is 8 deep
            CHECK(phys_to_virt(value) != NULL);                     // Only ever pages it was given
            fake.txq[fake.tx_tail++ % FAKE_FIFO] = value;
            break;
        case AR_Q_TXD:
            if (value & 1) fake.tx_head = fake.tx_tail;            // Stop: what's queued is dropped
            break;
        case AR_Q_STATUS_RING_START:
            fake.status_slot = 0;
            break;
        case AR_PHY_65NM_CH0_SYNTH7: {
            uint32_t chansel = (value & ~(AR9380_FRACMODE | AR9380_LOAD_SYNTH)) >> 2;
            uint32_t freq = (chansel * 15 + 215 + 0xFFFF) >> 16;
            fake.channel = (uint8_t)((freq - 2407) / 5);
            break;
        }
    }
}

static uint64_t now_ms = 0;
static uint32_t fake_us = 0;
// The card sends what's queued as time passes, besides when the driver sleeps
static void fake_tx_run(void);
static void fake_tick(void) {
    if (++fake_us >= 1000) { fake_us = 0; now_ms++; }
    if (fake_us % 50 == 0 && fake.tx_head != fake.tx_tail) fake_tx_run();
}

// Is the PCU receiving? (rx_start cleared RX_DIS and set the filter)
static bool fake_receiving(void) {
    return fake.rx_dma && !(fake.regs[AR_DIAG_SW / 4] & AR_DIAG_RX_DIS) &&
           (fake.regs[AR_RX_FILTER / 4] & AR_RX_FILTER_BEACON);
}

// The card writes one frame into the next low-priority (or high) buffer.
// Status words as the AR9003 writes them; `len` includes a 4-byte FCS.
static bool fake_deliver_raw(bool hp, const uint32_t* status, const uint8_t* frame, uint32_t copy_len) {
    uint32_t* head = hp ? &fake.hp_head : &fake.lp_head;
    uint32_t* tail = hp ? &fake.hp_tail : &fake.lp_tail;
    uint32_t* fifo = hp ? fake.hp : fake.lp;
    if (*head == *tail) return false;          // Overrun: dropped
    uint8_t* page = phys_to_virt(fifo[(*head)++ % FAKE_FIFO]);
    CHECK(page != NULL);
    memcpy(page + AR9485_RX_STATUS_SIZE, frame, copy_len);
    memcpy(page, status, AR9485_RX_STATUS_SIZE);
    return true;
}

static void make_status(uint32_t* st, uint32_t len, int8_t rssi, uint32_t flags11, uint32_t extra2) {
    memset(st, 0, AR9485_RX_STATUS_SIZE);
    st[0] = SM(AR_RXI_DESC_ID, AR_VENDOR_ATHEROS);
    st[1] = SM(AR_RXS1_RATE, 0x0b);                 // OFDM 6 Mbit/s
    st[2] = len | extra2;
    st[5] = (uint32_t)(uint8_t)rssi << 24;
    st[11] = AR_RXS11_DONE | flags11;
}

static bool fake_deliver(bool hp, const uint8_t* frame, uint32_t len, int8_t rssi) {
    uint8_t buf[4096];
    memcpy(buf, frame, len);
    memset(buf + len, 0xEE, 4);                     // FCS
    uint32_t st[12];
    make_status(st, len + 4, rssi, AR_RXS11_FRAME_OK, 0);
    return fake_deliver_raw(hp, st, buf, len + 4);
}

static void fake_reset(void) {
    memset(&fake, 0, sizeof(fake));
    fake.srev = FAKE_SREV;
    fake.die_after = -1;
}

// The air: frames for the station wait here until the card receives them
// (no recursion into the driver)
#define AIRQ_MAX 32
static struct { uint8_t data[2400]; uint32_t len; } airq[AIRQ_MAX];
static int airq_count = 0;
static void air_to_sta(const uint8_t* f, uint32_t len) {
    if (airq_count < AIRQ_MAX) { memcpy(airq[airq_count].data, f, len); airq[airq_count].len = len; airq_count++; }
}

static bool same6(const uint8_t* a, const uint8_t* b) { return memcmp(a, b, 6) == 0; }

// Would the PCU take this frame, as its filter and addresses are set?
static bool fake_accepts(const uint8_t* f, uint32_t len) {
    if (len < 24) return false;
    uint32_t filt = fake.regs[AR_RX_FILTER / 4];
    if (f[0] == 0x80) return (filt & AR_RX_FILTER_BEACON) != 0;
    uint8_t me[6], bss[6];
    uint32_t id0 = fake.regs[AR_STA_ID0 / 4], id1 = fake.regs[AR_STA_ID1 / 4];
    uint32_t b0 = fake.regs[AR_BSS_ID0 / 4], b1 = fake.regs[AR_BSS_ID1 / 4];
    for (int i = 0; i < 4; i++) { me[i] = (uint8_t)(id0 >> (8 * i)); bss[i] = (uint8_t)(b0 >> (8 * i)); }
    me[4] = (uint8_t)id1; me[5] = (uint8_t)(id1 >> 8); bss[4] = (uint8_t)b1; bss[5] = (uint8_t)(b1 >> 8);
    if (same6(f + 4, me)) return (filt & AR_RX_FILTER_UCAST) != 0;
    if (f[4] & 1) {
        bool bcast = f[4] == 0xff && f[5] == 0xff;
        return (filt & (bcast ? AR_RX_FILTER_BCAST : AR_RX_FILTER_MCAST)) && same6(f + 10, bss);
    }
    return false;
}

// The card receives: the PCU's filter, the 2-byte pad after a QoS data
// header (as the AR9003 writes it), then a receive buffer
static void fake_air_deliver(void) {
    for (int i = 0; i < airq_count; i++) {
        const uint8_t* f = airq[i].data; uint32_t len = airq[i].len;
        if (!fake_receiving() || !fake_accepts(f, len)) continue;
        uint8_t buf[2600];
        uint32_t hdr = ((f[0] & 0x0c) == 0x08 && (f[0] & 0x80)) ? 26 : 0;
        if (hdr) { memcpy(buf, f, hdr); buf[hdr] = 0xA5; buf[hdr + 1] = 0x5A; memcpy(buf + hdr + 2, f + hdr, len - hdr); len += 2; }
        else memcpy(buf, f, len);
        memset(buf + len, 0xEE, 4);
        uint32_t st[12];
        make_status(st, len + 4, 50, AR_RXS11_FRAME_OK, 0);
        fake_deliver_raw(false, st, buf, len + 4);
    }
    airq_count = 0;
}

static bool ap_rx(const uint8_t* f, uint32_t len);     // The access point hears a frame: true if it acknowledges

// The card sends what's queued: checks every descriptor as the AR9003
// reads it, hands the frame to the air and writes a status into the ring
static void fake_tx_run(void) {
    while (!fake.tx_stall && fake.tx_head != fake.tx_tail) {
        uint32_t addr = fake.txq[fake.tx_head++ % FAKE_FIFO];
        uint8_t* page = phys_to_virt(addr);
        CHECK(page != NULL);
        uint32_t d[32];
        memcpy(d, page, sizeof(d));
        CHECK(MS(d[0], AR_TXI_DESC_ID) == AR_VENDOR_ATHEROS && MS(d[0], AR_TXI_DESC_NDWORDS) == 23);
        CHECK((d[0] & AR_TXI_DESC_TX) && (d[0] & AR_TXI_CTRL_STAT) && MS(d[0], AR_TXI_QCU_NUM) == 0);
        uint32_t sum = 0; for (int i = 0; i < 10; i++) sum += d[i];
        CHECK(MS(d[10], AR_TXC10_PTR_CHK_SUM) == (((sum >> 16) + (sum & 0xffff)) & 0xffff));
        CHECK(d[1] == 0 && d[4] == 0 && d[5] == 0);                 // One buffer, no link
        uint32_t blen = MS(d[3], AR_TXC_BUF_LEN);
        CHECK(MS(d[11], AR_TXC11_FRAME_LEN) == blen + 4);
        CHECK(d[2] >= addr + 128 && d[2] + blen <= addr + 4096);  // The frame lies in the same page
        CHECK(MS(d[11], AR_TXC11_XMIT_POWER) == 63 && (d[11] & AR_TXC11_CLR_DEST_MASK));
        CHECK(MS(d[17], AR_TXC17_ENCR_TYPE) == 0 && d[19] == AR_TXC19_NOT_SOUNDING);
        CHECK(MS(d[18], AR_TXC18_CHAIN_SEL0) == 1);
        CHECK(MS(d[13], AR_TXC13_XMIT_DATA_TRIES0) >= 1);
        const uint8_t* f = page + (d[2] - addr);
        struct Sent* sn = &sent[sent_count % 64];
        memcpy(sn->frame, f, blen); sn->len = blen; memcpy(sn->ctl, d, sizeof(d));
        bool group = f[4] & 1;
        CHECK(group == ((d[12] & AR_TXC12_NO_ACK) != 0));
        bool acked = ap_rx(f, blen) && !group && !fake.tx_deaf;
        sn->acked = acked;
        sent_count++;
        uint32_t start = fake.regs[AR_Q_STATUS_RING_START / 4], end = fake.regs[AR_Q_STATUS_RING_END / 4];
        CHECK(end > start && (end - start) % 36 == 0);
        uint32_t slots = (end - start) / 36;
        uint32_t at = start + (fake.status_slot % slots) * 36;
        uint8_t* sp = phys_to_virt(at & ~0xfffU);
        CHECK(sp != NULL);
        uint32_t st[9] = { 0 };
        st[0] = SM(AR_TXI_DESC_ID, AR_VENDOR_ATHEROS) | AR_TXI_DESC_TX | SM(AR_TXI_QCU_NUM, 0);
        st[3] = acked || group ? 0x1 : AR_TXS3_EXCESSIVE_RETRIES;
        st[8] = AR_TXS8_DONE;
        memcpy(sp + (at & 0xfff), st, sizeof(st));
        fake.status_slot++;
    }
}

//
// ROM images
//

// Put `len` bytes at the top of the ROM going down from `*addr`, as the
// driver reads them (byte at addr first)
static void rom_put(uint32_t* addr, const void* data, uint32_t len) {
    const uint8_t* d = (const uint8_t*)data;
    for (uint32_t i = 0; i < len; i++) { CHECK(*addr < sizeof(fake.rom)); fake.rom[(*addr)--] = d[i]; }
}

// One block: header (alg, ref, len), data, checksum
static void rom_block(uint32_t* addr, uint8_t alg, uint8_t ref, const uint8_t* data, uint32_t len, bool good_sum) {
    uint8_t hdr[4];
    hdr[0] = (uint8_t)((alg << 5) | (ref & 0x1f));
    hdr[1] = (uint8_t)(((ref & 0x20) << 2) | ((len >> 4) & 0x7f));
    hdr[2] = (uint8_t)((len & 0xf) << 4);
    hdr[3] = 0;
    rom_put(addr, hdr, 4);
    rom_put(addr, data, len);
    uint16_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (uint16_t)(sum + data[i]);
    if (!good_sum) sum ^= 0x5a5a;
    uint8_t s[2] = { (uint8_t)sum, (uint8_t)(sum >> 8) };
    rom_put(addr, s, 2);
}

static const uint8_t test_mac[6] = { 0x00, 0x13, 0x74, 0x12, 0x34, 0x56 };

// A typical card: a compressed block against template 2 that sets the MAC
// and turns the wireless switch GPIO on
static void rom_typical(int kind, uint32_t size, bool switch_gpio) {
    fake.rom_kind = kind;
    fake.rom_size = size;
    memset(fake.rom, 0xff, sizeof(fake.rom));
    uint32_t addr = size - 1;
    uint8_t chunks[64]; uint32_t n = 0;
    chunks[n++] = 2; chunks[n++] = 6;               // macAddr at offset 2
    memcpy(chunks + n, test_mac, 6); n += 6;
    if (switch_gpio) {
        // rfSilent: offset 8 + 20 + 7 = 35; enabled, GPIO 3 -> 0x0d
        chunks[n++] = (uint8_t)(35 - 8); chunks[n++] = 1; chunks[n++] = 0x01 | (3 << 2);
    }
    rom_block(&addr, AR_EEP_COMPRESS_BLOCK, 2, chunks, n, true);
    uint8_t end[4] = { 0, 0, 0, 0 };
    rom_put(&addr, end, 4);
}

//
// Kernel stubs
//

void mutex_init(mutex_t* m) { memset(m, 0, sizeof(*m)); }
void mutex_lock(mutex_t* m) { CHECK(!m->locked); m->locked = true; }
void mutex_unlock(mutex_t* m) { CHECK(m->locked); m->locked = false; }

uint64_t timer_get_ms(void) { return now_ms; }

// Sleeping is when the air delivers: queued beacons for the tuned channel,
// what the card sends, and the access point's answers and beacons
struct Air { uint8_t channel; uint8_t frame[256]; uint32_t len; int8_t rssi; };
static struct Air air[8]; static int air_count = 0;
static int sleeps = 0;
static void ap_tick(void);
void thread_sleep(uint32_t ms) {
    now_ms += ms;
    sleeps++;
    if (fake_receiving()) {
        for (int i = 0; i < air_count; i++)
            if (air[i].channel == fake.channel) fake_deliver(i & 1, air[i].frame, air[i].len, air[i].rssi);
    }
    fake_tx_run();
    ap_tick();
    fake_air_deliver();
}

static char log_text[16384]; static size_t log_len = 0;
void print(const char* s) {
    size_t n = strlen(s);
    if (log_len + n < sizeof(log_text)) { memcpy(log_text + log_len, s, n); log_len += n; log_text[log_len] = 0; }
}

static ProcessId cur_pid = 0;
static ProcessSecurity cur_sec = 0;
ProcessId process_current_id(void) { return cur_pid; }
ProcessSecurity process_current_security(void) { return cur_sec; }

static int chardevs = 0;
static struct CharDevice* cds[8];
struct CharDevice* chardev_register(const char* name, const struct CharDeviceOps* ops, void* d, uint32_t sec) {
    struct CharDevice* cd = calloc(1, sizeof(*cd));
    strncpy(cd->name, name, CHARDEV_NAME_MAX - 1);
    cd->ops = ops; cd->driver_data = d; cd->security = sec;
    for (int i = 0; i < 8; i++) if (cds[i] == NULL) { cds[i] = cd; break; }
    chardevs++;
    return cd;
}
void chardev_unregister(struct CharDevice* cd) {
    if (!cd) return;
    for (int i = 0; i < 8; i++) if (cds[i] == cd) cds[i] = NULL;
    chardevs--; free(cd);
}
static struct CharDevice* find_cd(const char* name) {
    for (int i = 0; i < 8; i++) if (cds[i] && strcmp(cds[i]->name, name) == 0) return cds[i];
    return NULL;
}
static uint8_t user_mem[4096];
bool chardev_copy_to_user(uint32_t a, const void* s, uint32_t n) {
    if (a + n > sizeof(user_mem)) return false;
    memcpy(user_mem + a, s, n); return true;
}
bool chardev_copy_from_user(void* d, uint32_t a, uint32_t n) {
    if (a + n > sizeof(user_mem)) return false;
    memcpy(d, user_mem + a, n); return true;
}

static int netifs = 0;
static uint8_t netif_mac[6]; static bool netif_mac_set = false;
static struct NetIf* the_netif = NULL;
struct NetIf* netif_register(const char* name, const struct NetIfOps* ops, void* d, const uint8_t* mac, uint16_t mtu) {
    (void)name; (void)mtu;
    CHECK(ops->transmit(NULL, "", 0) == NET_ERR_DOWN);
    if (mac) { memcpy(netif_mac, mac, 6); netif_mac_set = true; }
    netifs++;
    struct NetIf* ifp = calloc(1, sizeof(struct NetIf));
    ifp->ops = ops; ifp->driver_data = d;
    the_netif = ifp;
    return ifp;
}
void netif_unregister(struct NetIf* ifp) {
    if (ifp) { netifs--; free(ifp); if (ifp == the_netif) the_netif = NULL; }
}
// The IP stack's side: link changes and frames received
static int link_changes = 0;
static uint8_t rx_eth[2000]; static uint32_t rx_eth_len = 0; static int rx_eth_count = 0;
void netif_set_link(struct NetIf* ifp, bool link) {
    CHECK(ifp != NULL);
    // Never called with the driver's station lock held (it takes the net lock)
    struct AthnAdapter* a = (struct AthnAdapter*)ifp->driver_data;
    CHECK(!a->sta_lock.locked);
    if (link) ifp->flags |= NETIF_LINK; else ifp->flags &= (uint16_t)~NETIF_LINK;
    link_changes++;
}
void netif_rx(struct NetIf* ifp, const void* frame, uint32_t length) {
    CHECK(ifp != NULL && length <= sizeof(rx_eth));
    memcpy(rx_eth, frame, length); rx_eth_len = length; rx_eth_count++;
}

// PCI
static uint16_t pci_cmd = 0;
static bool map_fails = false;
void pci_enable_command_bits(uint8_t b, uint8_t d, uint8_t f, uint16_t bits) { (void)b; (void)d; (void)f; pci_cmd |= bits; }
uint16_t pci_read_command(uint8_t b, uint8_t d, uint8_t f) { (void)b; (void)d; (void)f; return pci_cmd; }
void pci_write_command(uint8_t b, uint8_t d, uint8_t f, uint16_t c) { (void)b; (void)d; (void)f; pci_cmd = c; }
static uint8_t fake_window[16];
void* pci_map_bar(uint8_t b, uint8_t d, uint8_t f, uint8_t bar, uint32_t max, uint32_t flags) {
    (void)b; (void)d; (void)f; (void)flags;
    CHECK(bar == 0 && max == ATHN_MMIO_SIZE);
    return map_fails ? NULL : fake_window;          // Never touched: the fake accessors stand in
}

// Pages
static uint32_t next_phys = 0x100000;
static bool pages_fail_at_10 = false;
void* vmm_alloc_pages(size_t n) {
    CHECK(n == 1);
    if (pages_fail_at_10 && pages_live >= 10) return NULL;
    for (int i = 0; i < MAX_PAGES; i++) if (pages_virt[i] == NULL) {
        pages_virt[i] = aligned_alloc(4096, 4096);
        pages_phys[i] = next_phys; next_phys += 0x1000;
        pages_live++;
        return pages_virt[i];
    }
    return NULL;
}
void vmm_free_pages(void* v, size_t n) {
    CHECK(n == 1);
    for (int i = 0; i < MAX_PAGES; i++) if (pages_virt[i] == v) {
        // The card must not be able to read or write here any more
        CHECK(!(pci_cmd & PCI_COMMAND_BUS_MASTER));
        CHECK(!fake.rx_dma);
        CHECK(fake.tx_head == fake.tx_tail);
        free(v); pages_virt[i] = NULL; pages_live--; return;
    }
    CHECK(0);
}
uint32_t vmm_get_phys_addr(void* v) {
    for (int i = 0; i < MAX_PAGES; i++) if (pages_virt[i] == v) return pages_phys[i];
    return 0;
}

// Driver core: just this driver
void driver_lock(void) {}
void driver_unlock(void) {}
void driver_tree_update(void) {}
uint32_t driver_count(void) { return 1; }
const struct Driver* driver_get(uint32_t i) { return i == 0 ? &athn_pci_driver : NULL; }
static bool mn(uint16_t w, uint16_t h) { return w == MATCH_ANY || w == h; }
bool driver_matches(const struct Driver* drv, const struct Device* dev) {
    for (const struct DeviceMatch* r = drv->match; r->bus != 0; r++)
        if (r->bus == dev->bus && mn(r->vendor, dev->vendor) && mn(r->product, dev->product) &&
            mn(r->class_code, dev->class_code) && mn(r->subclass, dev->subclass) && mn(r->protocol, dev->protocol))
            return true;
    return false;
}

//
// The access point: on one channel, beaconing every 100 ms, answering
// authentication and association, running the WPA2 4-way handshake and
// moving data. Written from the standard, separately from the station
// (tests/wlan_sta has the same kind of access point for the station alone).
//

static uint32_t rnd(void);
static const uint8_t AP_MAC[6] = { 0x0A, 0xBB, 0xCC, 0xDD, 0xEE, 0x01 };
static const uint8_t ROUTER[6] = { 0x0A, 0xBB, 0xCC, 0xDD, 0xEE, 0x99 };     // Behind the AP
static const uint8_t BCAST[6]  = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static void ap_put16le(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void ap_put16be(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t ap_get16le(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint16_t ap_get16be(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

static struct {
    bool     on_air;
    uint8_t  channel;
    char     ssid[33];
    bool     open;
    bool     g_rates;                   // 802.11g rates offered (else 11b only)
    uint8_t  pmk[32];
    uint8_t  anonce[32];
    uint64_t replay;
    wpa_ptk_t ptk;
    bool     keys;
    aes128_t tk, gtk;
    uint8_t  gtk_raw[16];
    uint64_t tx_pn, gtk_pn, rx_pn;
    uint16_t seq;
    uint64_t last_beacon;
    bool     associated;
    int      auths, assocs, m2s, m4s, deauths, data_in, bad_mic;
    uint8_t  got[2400];                 // The last data frame's payload (after the LLC header)
    uint32_t got_len;
    uint16_t got_type;
} ap;

static uint32_t ap_header(uint8_t* p, uint8_t fc0, uint8_t fc1, const uint8_t* a1, const uint8_t* a3) {
    p[0] = fc0; p[1] = fc1; p[2] = p[3] = 0;
    memcpy(p + 4, a1, 6); memcpy(p + 10, AP_MAC, 6); memcpy(p + 16, a3, 6);
    ap_put16le(p + 22, (uint16_t)(ap.seq++ << 4));
    return 24;
}

static uint32_t ap_rsn(uint8_t* p) {
    static const uint8_t ie[] = { 48, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4,
                                  1, 0, 0x00, 0x0F, 0xAC, 2, 0, 0 };
    memcpy(p, ie, sizeof(ie));
    return sizeof(ie);
}

static void ap_beacon(void) {
    uint8_t f[256];
    uint32_t n = ap_header(f, 0x80, 0, BCAST, AP_MAC);
    memset(f + n, 0, 8); n += 8;
    ap_put16le(f + n, 100); n += 2;
    ap_put16le(f + n, (uint16_t)(0x0001 | (ap.open ? 0 : 0x0010))); n += 2;
    f[n++] = 0; f[n++] = (uint8_t)strlen(ap.ssid); memcpy(f + n, ap.ssid, strlen(ap.ssid)); n += (uint32_t)strlen(ap.ssid);
    if (ap.g_rates) {
        // 1, 2, 5.5, 11 basic; 6, 9, 12, 18, then 24, 36, 48, 54 extended
        static const uint8_t r[] = { 1, 8, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24, 50, 4, 0x30, 0x48, 0x60, 0x6C };
        memcpy(f + n, r, sizeof(r)); n += sizeof(r);
    } else {
        static const uint8_t r[] = { 1, 4, 0x82, 0x84, 0x0B, 0x16 };
        memcpy(f + n, r, sizeof(r)); n += sizeof(r);
    }
    f[n++] = 3; f[n++] = 1; f[n++] = ap.channel;
    if (!ap.open) n += ap_rsn(f + n);
    air_to_sta(f, n);
}

// RFC 3394 key wrap, for the GTK in message 3
static void ap_wrap(const uint8_t kek[16], const uint8_t* in, size_t len, uint8_t* out) {
    aes128_t k; aes128_init(&k, kek);
    size_t n = len / 8; uint8_t A[8], b[16];
    memset(A, 0xA6, 8); memcpy(out + 8, in, len);
    for (int j = 0; j <= 5; j++)
        for (size_t i = 1; i <= n; i++) {
            memcpy(b, A, 8); memcpy(b + 8, out + 8 * i, 8);
            aes128_encrypt(&k, b, b);
            uint64_t t = (uint64_t)n * j + i;
            memcpy(A, b, 8);
            for (int q = 0; q < 8; q++) A[7 - q] ^= (uint8_t)(t >> (8 * q));
            memcpy(out + 8 * i, b + 8, 8);
        }
    memcpy(out, A, 8);
}

// Data from the AP: QoS data (TID 0) to the station, plain data to a group,
// encrypted once keys are set
static void ap_data(const uint8_t* dst, const uint8_t* src, uint16_t type, const uint8_t* payload, uint32_t len, bool protect) {
    uint8_t f[2400];
    bool group = dst[0] & 1;
    bool qos = !group;
    uint32_t n = ap_header(f, qos ? 0x88 : 0x08, (uint8_t)(0x02 | (protect ? 0x40 : 0)), dst, src);
    if (qos) { f[n++] = 0; f[n++] = 0; }
    uint8_t body[2000];
    static const uint8_t llc[6] = { 0xAA, 0xAA, 0x03, 0, 0, 0 };
    memcpy(body, llc, 6); ap_put16be(body + 6, type); memcpy(body + 8, payload, len);
    if (!protect) { memcpy(f + n, body, 8 + len); air_to_sta(f, n + 8 + len); return; }
    uint64_t pn = group ? ++ap.gtk_pn : ++ap.tx_pn;
    CHECK(ccmp_encrypt(group ? &ap.gtk : &ap.tk, f, n, pn, group ? 1 : 0, body, 8 + len, f + n));
    air_to_sta(f, n + 8 + 8 + len + 8);
}

static void ap_key(uint16_t info, const uint8_t* data, uint32_t data_len) {
    uint8_t e[512];
    memset(e, 0, 99);
    e[0] = 2; e[1] = 3; ap_put16be(e + 2, (uint16_t)(95 + data_len));
    e[4] = 2; ap_put16be(e + 5, (uint16_t)(info | 2)); ap_put16be(e + 7, 16);
    ap.replay++;
    for (int i = 0; i < 8; i++) e[9 + i] = (uint8_t)(ap.replay >> (56 - 8 * i));
    memcpy(e + 17, ap.anonce, 32);
    ap_put16be(e + 97, (uint16_t)data_len);
    if (data_len) memcpy(e + 99, data, data_len);
    if (info & 0x0100) wpa_mic(ap.ptk.kck, e, 99 + data_len, e + 81);
    ap_data(test_mac, AP_MAC, 0x888E, e, 99 + data_len, false);
}

static bool ap_mic_ok(uint8_t* e, uint32_t len) {
    uint8_t want[16], got[16];
    memcpy(want, e + 81, 16); memset(e + 81, 0, 16);
    wpa_mic(ap.ptk.kck, e, len, got);
    memcpy(e + 81, want, 16);
    return memcmp(want, got, 16) == 0;
}

static void ap_eapol(uint8_t* e, uint32_t len) {
    uint16_t info = ap_get16be(e + 5);
    if ((info & 0x0008) && !(info & 0x0200)) {             // Message 2
        ap.m2s++;
        wpa_ptk(ap.pmk, AP_MAC, test_mac, ap.anonce, e + 17, &ap.ptk);
        if (!ap_mic_ok(e, len)) { ap.bad_mic++; return; }  // Wrong password: silence
        uint8_t plain[64], wrapped[72]; uint32_t n = ap_rsn(plain);
        static const uint8_t kde[8] = { 0xDD, 22, 0x00, 0x0F, 0xAC, 0x01, 1, 0 };
        memcpy(plain + n, kde, 8); n += 8; memcpy(plain + n, ap.gtk_raw, 16); n += 16;
        if (n % 8) { plain[n++] = 0xDD; while (n % 8) plain[n++] = 0; }
        ap_wrap(ap.ptk.kek, plain, n, wrapped);
        ap_key(0x0008 | 0x0040 | 0x0080 | 0x0100 | 0x0200 | 0x1000, wrapped, n + 8);
    } else if ((info & 0x0008) && (info & 0x0200)) {       // Message 4
        ap.m4s++;
        CHECK(ap_mic_ok(e, len));
        ap.keys = true;
        aes128_init(&ap.tk, ap.ptk.tk);
    }
}

static void ap_reset(const char* ssid, const char* pass, uint8_t channel) {
    memset(&ap, 0, sizeof(ap));
    strcpy(ap.ssid, ssid);
    ap.open = pass == NULL;
    ap.g_rates = true;
    ap.channel = channel;
    ap.on_air = true;
    if (pass) CHECK(wpa_pmk(pass, strlen(pass), (const uint8_t*)ssid, strlen(ssid), ap.pmk));
    memset(ap.gtk_raw, 0x5A, 16);
    aes128_init(&ap.gtk, ap.gtk_raw);
    airq_count = 0;
}

static void ap_tick(void) {
    if (!ap.on_air || fake.channel != ap.channel) return;
    if (now_ms - ap.last_beacon >= 100) { ap.last_beacon = now_ms; ap_beacon(); }
}

static bool ap_rx(const uint8_t* f, uint32_t len) {
    if (!ap.on_air || fake.channel != ap.channel || len < 24) return false;
    if (!same6(f + 4, AP_MAC)) return false;
    CHECK(same6(f + 10, test_mac));
    const uint8_t* b = f + 24;
    uint8_t r[64];
    switch (f[0]) {
        case 0xB0: {                                       // Authentication: open system, transaction 1
            CHECK(len >= 30 && ap_get16le(b) == 0 && ap_get16le(b + 2) == 1);
            ap.auths++;
            uint32_t n = ap_header(r, 0xB0, 0, test_mac, AP_MAC);
            ap_put16le(r + n, 0); ap_put16le(r + n + 2, 2); ap_put16le(r + n + 4, 0);
            air_to_sta(r, n + 6);
            return true;
        }
        case 0x00: {                                       // Association request
            ap.assocs++;
            CHECK(b[4] == 0 && b[5] == strlen(ap.ssid) && memcmp(b + 6, ap.ssid, b[5]) == 0);
            uint32_t n = ap_header(r, 0x10, 0, test_mac, AP_MAC);
            ap_put16le(r + n, 0x0011); ap_put16le(r + n + 2, 0); ap_put16le(r + n + 4, 0xC001);
            air_to_sta(r, n + 6);
            ap.associated = true;
            if (!ap.open) {
                for (int i = 0; i < 32; i++) ap.anonce[i] = (uint8_t)rnd();
                ap_key(0x0008 | 0x0080, NULL, 0);          // Message 1
            }
            return true;
        }
        case 0xC0: case 0xA0:
            ap.deauths++; ap.associated = false; ap.keys = false;
            return true;
        case 0x08: {
            CHECK((f[1] & 0x03) == 0x01);                  // To the DS
            uint8_t body[2400]; size_t blen;
            if (f[1] & 0x40) {
                CHECK(ap.keys);
                uint64_t pn; uint8_t id;
                CHECK(ccmp_decrypt(&ap.tk, f, len, 24, body, &blen, &pn, &id));
                CHECK(pn > ap.rx_pn); ap.rx_pn = pn;
            } else {
                blen = len - 24; memcpy(body, b, blen);
            }
            CHECK(blen >= 8 && body[0] == 0xAA && body[1] == 0xAA);
            uint16_t type = ap_get16be(body + 6);
            if (type == 0x888E) { ap_eapol(body + 8, (uint32_t)(blen - 8)); return true; }
            CHECK(ap.open || (f[1] & 0x40));               // Never in the clear on WPA2
            ap.data_in++;
            ap.got_type = type; ap.got_len = (uint32_t)(blen - 8); memcpy(ap.got, body + 8, blen - 8);
            return true;
        }
    }
    return true;
}

//
// Helpers
//

static uint32_t beacon(uint8_t* f, const uint8_t* bssid, const char* ssid, uint8_t channel, uint16_t caps) {
    memset(f, 0, 36);
    f[0] = 0x80;
    memset(f + 4, 0xFF, 6); memcpy(f + 10, bssid, 6); memcpy(f + 16, bssid, 6);
    f[32] = 100;
    f[34] = (uint8_t)caps; f[35] = (uint8_t)(caps >> 8);
    uint32_t n = 36;
    size_t sl = strlen(ssid);
    f[n++] = 0; f[n++] = (uint8_t)sl; memcpy(f + n, ssid, sl); n += (uint32_t)sl;
    f[n++] = 3; f[n++] = 1; f[n++] = channel;       // DS parameter set
    return n;
}

// A chip with its own receive pages, for the chip-level tests
static struct Ar9485 chip;
static uint8_t eep_image[AR9485_EEP_IMAGE_SIZE];
static void chip_free(void) {
    fake.rx_dma = false; pci_cmd = 0; fake.tx_head = fake.tx_tail;
    for (int i = 0; i < AR9485_RX_COUNT; i++)
        if (chip.rx[i].virt) { vmm_free_pages(chip.rx[i].virt, 1); chip.rx[i].virt = NULL; }
    for (int i = 0; i < AR9485_TX_COUNT; i++)
        if (chip.tx[i].virt) { vmm_free_pages(chip.tx[i].virt, 1); chip.tx[i].virt = NULL; }
    if (chip.tx_status.virt) { vmm_free_pages(chip.tx_status.virt, 1); chip.tx_status.virt = NULL; }
}
// Transmit pages too: the chip can send
static void chip_tx_pages(void) {
    for (int i = 0; i < AR9485_TX_COUNT; i++) {
        chip.tx[i].virt = vmm_alloc_pages(1); memset(chip.tx[i].virt, 0, 4096);
        chip.tx[i].phys = vmm_get_phys_addr(chip.tx[i].virt);
    }
    chip.tx_status.virt = vmm_alloc_pages(1); memset(chip.tx_status.virt, 0, 4096);
    chip.tx_status.phys = vmm_get_phys_addr(chip.tx_status.virt);
}
static void chip_init(void) {
    chip_free();
    memset(&chip, 0, sizeof(chip));
    chip.bus.ctx = NULL; chip.bus.read = fake_read; chip.bus.write = fake_write; chip.bus.delay_us = athn_delay_us;
    for (int i = 0; i < AR9485_RX_COUNT; i++) {
        chip.rx[i].virt = vmm_alloc_pages(1);
        chip.rx[i].phys = vmm_get_phys_addr(chip.rx[i].virt);
    }
}

struct Got { int n; struct WlanRxFrame f[64]; uint8_t copy[64][512]; };
static void got_fn(void* ctx, const struct WlanRxFrame* f) {
    struct Got* g = (struct Got*)ctx;
    // The frame handed over is never DMA memory
    for (int i = 0; i < AR9485_RX_COUNT; i++)
        CHECK(!(f->data >= chip.rx[i].virt && f->data < chip.rx[i].virt + 4096));
    volatile uint8_t x = 0; for (uint32_t i = 0; i < f->length; i++) x ^= f->data[i]; (void)x;
    if (g->n < 64) { g->f[g->n] = *f; memcpy(g->copy[g->n], f->data, f->length < 512 ? f->length : 512); }
    g->n++;
}
static void sink(void* ctx, const struct WlanRxFrame* f) {
    (void)ctx; struct WlanBss b;
    volatile uint8_t x = 0; for (uint32_t i = 0; i < f->length; i++) x ^= f->data[i]; (void)x;
    wlan_parse_beacon(f->data, f->length, 1, &b);
}

static struct WlanLink t_link(struct CharDevice* c) {
    CHECK(c->ops->ioctl(c, WLAN_IOCTL_LINK, 3000) == SYS_OK);
    struct WlanLink l;
    memcpy(&l, user_mem + 3000, sizeof(l));
    return l;
}

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    //
    // 0. The real MMIO accessors refuse offsets outside the window
    //
    {
        static uint32_t window[ATHN_MMIO_SIZE / 4];
        struct AthnAdapter fa;
        memset(&fa, 0, sizeof(fa));
        fa.regs = (uint8_t*)window;
        athn_reg_write(&fa, 0x4020, 0x1234);
        CHECK(athn_reg_read(&fa, 0x4020) == 0x1234);
        athn_reg_write(&fa, ATHN_MMIO_SIZE - 4, 7);
        CHECK(athn_reg_read(&fa, ATHN_MMIO_SIZE - 4) == 7);
        athn_reg_write(&fa, ATHN_MMIO_SIZE, 1);           // dropped
        athn_reg_write(&fa, 0x4021, 1);                   // dropped
        CHECK(athn_reg_read(&fa, ATHN_MMIO_SIZE) == 0xFFFFFFFFU);
        CHECK(athn_reg_read(&fa, 0xFFFFFFFCU) == 0xFFFFFFFFU);
        CHECK(athn_reg_read(&fa, 0x4022) == 0xFFFFFFFFU);
        CHECK(window[0x4020 / 4] == 0x1234);
        printf("ok 0 mmio bounds\n");
    }

    //
    // 1. Identification
    //
    fake_reset(); chip_init();
    CHECK(ar9485_identify(&chip));
    CHECK(chip.mac_ver == 0x240 && chip.mac_rev == 1);
    CHECK(chip.wa & AR_WA_D3_L1_DISABLE);
    fake.srev = 0xFFFFFFFFU; CHECK(!ar9485_identify(&chip));
    fake.srev = 0x001c00ffU; CHECK(!ar9485_identify(&chip));                       // AR9380
    CHECK(strcmp(chip.error, "not an AR9485") == 0);
    printf("ok 1: identify AR9485, reject others and a dead card\n");

    //
    // 2. ROM: EEPROM and OTP, compressed and whole images, bad blocks
    //
    fake_reset(); chip_init(); rom_typical(1, 4096, false);
    CHECK(ar9485_identify(&chip) && ar9485_attach(&chip, eep_image));
    CHECK(chip.rom_ok && !chip.rom_otp && chip.rom_size == 4096 && chip.rom_blocks == 1);
    CHECK(chip.mac_valid && memcmp(chip.mac, test_mac, 6) == 0);
    CHECK(!chip.rfsilent && chip.txmask == 1 && chip.rxmask == 1);

    fake_reset(); chip_init(); rom_typical(2, 1024, true);
    CHECK(ar9485_identify(&chip) && ar9485_attach(&chip, eep_image));
    CHECK(chip.rom_ok && chip.rom_otp && chip.rom_size == 1024);
    CHECK(chip.mac_valid && memcmp(chip.mac, test_mac, 6) == 0);
    CHECK(chip.rfsilent && chip.rfsilent_pin == 3 && !chip.rfsilent_reversed);

    // A whole image, uncompressed, in a 4 KB EEPROM
    fake_reset(); chip_init();
    fake.rom_kind = 1; fake.rom_size = 4096; memset(fake.rom, 0xff, sizeof(fake.rom));
    {
        uint8_t img[AR9485_EEP_IMAGE_SIZE]; memcpy(img, ar9380_def_rom_h112, sizeof(img));
        memcpy(img + 2, test_mac, 6);
        uint32_t addr = 4095; rom_block(&addr, AR_EEP_COMPRESS_NONE, 0, img, sizeof(img), true);
    }
    CHECK(ar9485_identify(&chip) && ar9485_attach(&chip, eep_image));
    CHECK(chip.rom_ok && chip.mac_valid && memcmp(eep_image, ar9380_def_rom_h112, 2) == 0);

    // Bad checksum: skipped, so no ROM and no MAC
    fake_reset(); chip_init();
    fake.rom_kind = 1; fake.rom_size = 4096; memset(fake.rom, 0xff, sizeof(fake.rom));
    { uint8_t c[8] = { 2, 6 }; memcpy(c + 2, test_mac, 6); uint32_t a = 4095; rom_block(&a, AR_EEP_COMPRESS_BLOCK, 2, c, 8, false); }
    CHECK(ar9485_identify(&chip) && ar9485_attach(&chip, eep_image));
    CHECK(!chip.rom_ok && !chip.mac_valid);

    // No ROM at all: attach still works, MAC unknown
    fake_reset(); chip_init();
    CHECK(ar9485_identify(&chip) && ar9485_attach(&chip, eep_image));
    CHECK(!chip.rom_ok && !chip.mac_valid);

    // Hostile chunks: past the image, a template we don't have, a length
    // longer than the image (skipped as a header), multicast MAC
    {
        static const struct { uint8_t alg, ref; uint8_t data[8]; uint32_t len; } bad[] = {
            { AR_EEP_COMPRESS_BLOCK, 2, { 255, 200 }, 2 },                       // chunk beyond the block
            { AR_EEP_COMPRESS_BLOCK, 9, { 2, 1, 0x00 }, 3 },                     // unknown template
            { AR_EEP_COMPRESS_NONE, 0, { 1, 2, 3 }, 3 },                         // whole image of the wrong size
            { AR_EEP_COMPRESS_PAIRS, 0, { 1, 2, 3 }, 3 },                        // compression we don't do
            { AR_EEP_COMPRESS_BLOCK, 2, { 2, 6, 0x01, 2, 3, 4, 5, 6 }, 8 },      // multicast MAC
        };
        for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            fake_reset(); chip_init();
            fake.rom_kind = 1; fake.rom_size = 4096; memset(fake.rom, 0xff, sizeof(fake.rom));
            uint32_t a = 4095; rom_block(&a, bad[i].alg, bad[i].ref, bad[i].data, bad[i].len, true);
            CHECK(ar9485_identify(&chip) && ar9485_attach(&chip, eep_image));
            CHECK(!chip.mac_valid);
        }
        // Gap pushes a chunk to the very end of the image: allowed up to the last byte, not past
        fake_reset(); chip_init();
        fake.rom_kind = 1; fake.rom_size = 4096; memset(fake.rom, 0xff, sizeof(fake.rom));
        uint8_t c[4 * 2 + 2 + 69];
        uint32_t n = 0;
        for (int k = 0; k < 4; k++) { c[n++] = 255; c[n++] = 0; }       // Gaps: offset 1020
        c[n++] = 0; c[n++] = 68; memset(c + n, 0x22, 68); n += 68;      // 1020 + 68 = 1088: exactly full
        uint32_t a = 4095; rom_block(&a, AR_EEP_COMPRESS_BLOCK, 2, c, n, true);
        CHECK(ar9485_identify(&chip) && ar9485_attach(&chip, eep_image));
        CHECK(chip.rom_ok && eep_image[1087] == 0x22 && eep_image[1019] == rom_template(2)[1019]);
        c[n - 69] = 69; c[n++] = 0x22;                                  // One byte too many
        fake_reset(); chip_init();
        fake.rom_kind = 1; fake.rom_size = 4096; memset(fake.rom, 0xff, sizeof(fake.rom));
        a = 4095; rom_block(&a, AR_EEP_COMPRESS_BLOCK, 2, c, n, true);
        CHECK(ar9485_identify(&chip) && ar9485_attach(&chip, eep_image));
        CHECK(!chip.rom_ok);
    }

    // A header that claims a block reaching below address 0 of a 512-byte OTP
    fake_reset(); chip_init();
    fake.rom_kind = 2; fake.rom_size = 512; memset(fake.rom, 0xff, sizeof(fake.rom));
    { uint8_t h[4] = { (AR_EEP_COMPRESS_BLOCK << 5) | 2, 0x40, 0x00, 0 }; uint32_t a = 511; rom_put(&a, h, 4); }   // len 1024
    fake.rom[1023] = 0x5a;                                                            // Make 1 KB OTP look absent
    CHECK(ar9485_identify(&chip) && ar9485_attach(&chip, eep_image));
    CHECK(!chip.rom_ok);

    // Fuzz: random ROM contents, both kinds
    for (int it = 0; it < 3000; it++) {
        fake_reset(); chip_init();
        fake.rom_kind = 1 + (it & 1); fake.rom_size = (it & 2) ? 4096 : 1024;
        for (uint32_t i = 0; i < sizeof(fake.rom); i++) fake.rom[i] = (uint8_t)rnd();
        // Often a plausible header on top
        if (it % 3) { uint32_t a = fake.rom_size - 1; uint8_t h[4] = { (uint8_t)((3 << 5) | (rnd() % 6)), (uint8_t)(rnd() & 0x7f), (uint8_t)rnd(), 0 }; rom_put(&a, h, 4); }
        CHECK(ar9485_identify(&chip) && ar9485_attach(&chip, eep_image));
        if (chip.mac_valid) CHECK(!(chip.mac[0] & 1));
        CHECK(chip.rfsilent_pin < 17);
    }
    printf("ok 2: ROM: EEPROM, OTP, whole image, bad checksum, hostile chunks and lengths, 3000 fuzzed\n");

    //
    // 3. Tuning
    //
    fake_reset(); chip_init(); rom_typical(1, 4096, false);
    CHECK(ar9485_identify(&chip) && ar9485_attach(&chip, eep_image));
    for (uint8_t ch = 1; ch <= 13; ch++) {
        CHECK(ar9485_tune(&chip, ch));
        CHECK(fake.channel == ch);
        CHECK(fake_receiving());
        CHECK(fake.lp_tail - fake.lp_head == AR9485_RX_LP_COUNT && fake.hp_tail - fake.hp_head == AR9485_RX_HP_COUNT);
        CHECK(fake.regs[AR_RX_FILTER / 4] == (AR_RX_FILTER_BCAST | AR_RX_FILTER_BEACON));
        CHECK(fake.regs[AR_IMR / 4] == 0 && fake.regs[AR_IER / 4] == 0);        // Polled: interrupts off
        CHECK(fake.regs[AR_PHY_RX_CHAINMASK / 4] == 1);
        CHECK((fake.regs[AR_STA_ID0 / 4] & 0xff) == test_mac[0] && ((fake.regs[AR_STA_ID0 / 4] >> 8) & 0xff) == test_mac[1]);
    }
    CHECK(!ar9485_tune(&chip, 0) && !ar9485_tune(&chip, 14));
    ar9485_stop(&chip);
    CHECK(!fake.rx_dma && !chip.rx_on);
    CHECK(fake.regs[AR_STA_ID1 / 4] & AR_STA_ID1_PWR_SAV);

    // The wireless switch off: nothing tuned, said so
    fake_reset(); chip_init(); rom_typical(2, 1024, true);
    CHECK(ar9485_identify(&chip) && ar9485_attach(&chip, eep_image));
    fake.switch_off = true;
    CHECK(!ar9485_tune(&chip, 6) && chip.switched_off && !fake_receiving());
    fake.switch_off = false;
    CHECK(ar9485_tune(&chip, 6) && !chip.switched_off);

    // AGC calibration never finishes
    fake.agc_stuck = true;
    CHECK(!ar9485_tune(&chip, 6) && strstr(chip.error, "AGC") != NULL);
    fake.agc_stuck = false;

    // The card dies at every point of a tune: it always ends, never with
    // receiving on, and gives up within a bounded number of accesses
    {
        fake_reset(); chip_init(); rom_typical(1, 4096, false);
        CHECK(ar9485_identify(&chip) && ar9485_attach(&chip, eep_image));
        long start = fake.accesses;
        CHECK(ar9485_tune(&chip, 3));
        long total = fake.accesses - start;
        long worst = 0;
        for (long k = 0; k < total; k++) {
            fake_reset(); chip_init(); rom_typical(1, 4096, false);
            CHECK(ar9485_identify(&chip) && ar9485_attach(&chip, eep_image));
            fake.die_after = fake.accesses + k;
            long before = fake.accesses;
            bool ok = ar9485_tune(&chip, 3);
            long after_death = fake.accesses - before - k;
            if (after_death > worst) worst = after_death;
            CHECK(!ok && !chip.rx_on);
            uint32_t got = ar9485_rx_harvest(&chip, sink, NULL);   // All ones status: never DONE-valid frames
            (void)got;
        }
        printf("ok 3: tune 1-13, switch off, stuck calibration, card dies at each of %ld accesses (worst %ld more)\n",
               total, worst);
        CHECK(worst <= 20000);   // The longest bounded poll (AGC: 10000) plus a few
    }

    //
    // 4. Receiving
    //
    fake_reset(); chip_init(); rom_typical(1, 4096, false);
    CHECK(ar9485_identify(&chip) && ar9485_attach(&chip, eep_image));
    CHECK(ar9485_tune(&chip, 6));
    {
        uint8_t f[256]; static const uint8_t b1[6] = { 0x9c, 0x53, 0x22, 1, 2, 3 };
        uint32_t len = beacon(f, b1, "HomeNetwork", 6, 0x0011);
        struct Got g; memset(&g, 0, sizeof(g));
        CHECK(fake_deliver(false, f, len, 47));
        CHECK(fake_deliver(true, f, len, 30));
        CHECK(ar9485_rx_harvest(&chip, got_fn, &g) == 2);
        CHECK(g.n == 2 && g.f[0].length == len && memcmp(g.copy[0], f, len) == 0);
        CHECK(g.f[0].rssi == 47 - 95 && g.f[1].rssi == 30 - 95 && !g.f[0].cck);
        // Buffers went back to the card
        CHECK(fake.lp_tail - fake.lp_head == AR9485_RX_LP_COUNT && fake.hp_tail - fake.hp_head == AR9485_RX_HP_COUNT);

        // Bad ones: CRC error, wrong descriptor id, a TX status, too short,
        // too long, split over buffers, invalid RSSI (kept, signal unknown)
        uint32_t st[12];
        uint32_t e0 = chip.rx_errors, b0 = chip.rx_bad;
        make_status(st, len + 4, 40, AR_RXS11_CRC_ERR, 0);              fake_deliver_raw(false, st, f, len);
        make_status(st, len + 4, 40, AR_RXS11_FRAME_OK, 0); st[0] = SM(AR_RXI_DESC_ID, 0x1234);  fake_deliver_raw(false, st, f, len);
        make_status(st, len + 4, 40, AR_RXS11_FRAME_OK, 0); st[0] |= AR_RXI_DESC_TX;             fake_deliver_raw(false, st, f, len);
        make_status(st, 10, 40, AR_RXS11_FRAME_OK, 0);                  fake_deliver_raw(false, st, f, len);
        make_status(st, 4000, 40, AR_RXS11_FRAME_OK, 0);                fake_deliver_raw(false, st, f, len);
        make_status(st, 0xfff, 40, AR_RXS11_FRAME_OK, 0);               fake_deliver_raw(false, st, f, len);
        make_status(st, len + 4, 40, AR_RXS11_FRAME_OK, AR_RXS2_MORE);  fake_deliver_raw(false, st, f, len);
        make_status(st, len + 4, -128, AR_RXS11_FRAME_OK, 0);           fake_deliver_raw(false, st, f, len + 4);
        memset(&g, 0, sizeof(g));
        CHECK(ar9485_rx_harvest(&chip, got_fn, &g) == 1);
        CHECK(g.f[0].rssi == WLAN_RSSI_UNKNOWN);
        CHECK(chip.rx_errors == e0 + 1 && chip.rx_bad == b0 + 6);
        CHECK(fake.lp_tail - fake.lp_head == AR9485_RX_LP_COUNT);

        // A card that marks everything done, forever: one pass per queue, no more
        for (int i = 0; i < AR9485_RX_COUNT; i++) {
            make_status(st, len + 4, 40, AR_RXS11_FRAME_OK, 0);
            memcpy(chip.rx[i].virt + AR9485_RX_STATUS_SIZE, f, len + 4);
            memcpy(chip.rx[i].virt, st, 48);
        }
        memset(&g, 0, sizeof(g));
        CHECK(ar9485_rx_harvest(&chip, got_fn, &g) <= AR9485_RX_COUNT);
        // That handed the card buffers it already had; a retune starts its FIFOs afresh
        CHECK(ar9485_tune(&chip, 6));
        CHECK(fake.lp_tail - fake.lp_head == AR9485_RX_LP_COUNT);

        // Fuzz: random status words and frames
        for (int it = 0; it < 100000; it++) {
            uint32_t rst[12]; uint8_t fr[512];
            for (int i = 0; i < 12; i++) rst[i] = rnd();
            if (it & 1) rst[0] = SM(AR_RXI_DESC_ID, AR_VENDOR_ATHEROS) | (rnd() & 0x3fff);
            rst[11] |= AR_RXS11_DONE;
            if (it & 2) { rst[11] |= AR_RXS11_FRAME_OK; rst[2] = rnd() % 520; }
            for (int i = 0; i < 512; i++) fr[i] = (uint8_t)rnd();
            if (it & 4) { fr[0] = 0x80; }
            fake_deliver_raw(it % 5 == 0, rst, fr, sizeof(fr));
            if (it % 7 == 0) ar9485_rx_harvest(&chip, sink, NULL);
        }
        ar9485_rx_harvest(&chip, sink, NULL);
        CHECK(fake.lp_tail - fake.lp_head <= AR9485_RX_LP_COUNT);
    }
    ar9485_stop(&chip);
    printf("ok 4: receive: beacons copied off DMA, RSSI, bad statuses dropped, endless DONE bounded, 100000 fuzzed\n");

    //
    // 5. The PCI driver: bind, /dev/wlan0, scan, permissions, remove
    //
    chip_free();
    CHECK(pages_live == 0);
    fake_reset(); rom_typical(1, 4096, false);
    struct PCIDeviceInfo pinfo = { .bus = 2, .dev = 0, .func = 0, .vendor_id = 0x168c, .device_id = 0x0032,
                                   .class_code = 0x02, .subclass = 0x80 };
    struct DeviceInfo di = { .bus = BUS_PCI, .vendor = 0x168c, .product = 0x0032, .class_code = 0x02,
                             .subclass = 0x80, .protocol = 0, .bus_data = &pinfo };
    struct Device* dev = device_add("pci2:00.0", NULL, &di);
    CHECK(dev != NULL && dev->driver == &athn_pci_driver);
    CHECK(chardevs == 1 && netifs == 1 && netif_mac_set && memcmp(netif_mac, test_mac, 6) == 0);
    CHECK(pages_live == ATHN_PAGE_COUNT);
    CHECK((pci_cmd & PCI_COMMAND_BUS_MASTER) && (pci_cmd & PCI_COMMAND_INTX_DISABLE));
    printf("%s", log_text);
    CHECK(strstr(log_text, "athn: wlan0: AR9485 rev 1 at pci 2:00.0") != NULL);
    CHECK(strstr(log_text, "athn: wlan0: MAC 00:13:74:12:34:56") != NULL);

    struct WlanInfo info;
    CHECK(wlan_info(0, &info) == SYS_OK && info.family == WLAN_FAMILY_AR9485 && info.mac_valid &&
          info.state == WLAN_STATE_IDENTIFIED && strcmp(info.chip, "AR9485") == 0 && info.vendor == 0x168c);

    // Permissions through /dev/wlan0: a process without the network bit
    struct CharDevice* cd = find_cd("wlan0");
    CHECK(cd != NULL && cd->security == SYS_SEC_NET);
    cur_pid = 7; cur_sec = 0;
    CHECK(cd->ops->open(cd, 0) == SYS_EPERM);
    CHECK(cd->ops->ioctl(cd, WLAN_IOCTL_SCAN, 0) == SYS_EPERM);
    CHECK(cd->ops->ioctl(cd, WLAN_IOCTL_INFO, 0) == SYS_EPERM);
    cur_sec = SYS_SEC_NET;
    CHECK(cd->ops->ioctl(cd, WLAN_IOCTL_INFO, 0) == 0);
    CHECK(cd->ops->ioctl(cd, 0x1234, 0) == SYS_ENOTTY);
    CHECK(cd->ops->ioctl(cd, WLAN_IOCTL_INFO, 4090) == SYS_EFAULT);

    // Air: two networks on 6, one on 11 with a hostile name
    {
        static const uint8_t b1[6] = { 0x9c, 0x53, 0x22, 1, 2, 3 }, b2[6] = { 0x9c, 0x53, 0x22, 1, 2, 4 }, b3[6] = { 2, 0x66, 0x66, 0x66, 0x66, 0x66 };
        air[0].channel = 6;  air[0].len = beacon(air[0].frame, b1, "HomeNetwork", 6, 0x0011);  air[0].rssi = 47;
        air[1].channel = 6;  air[1].len = beacon(air[1].frame, b2, "", 6, 0x0001);             air[1].rssi = 44;
        air[2].channel = 11; air[2].len = beacon(air[2].frame, b3, "Cafe\x1b[2J", 11, 0x0001);  air[2].rssi = 18;
        air_count = 3;
    }
    // From a process: started quietly
    CHECK(cd->ops->ioctl(cd, WLAN_IOCTL_SCAN, 0) == SYS_OK);
    CHECK(cd->ops->ioctl(cd, WLAN_IOCTL_SCAN, 0) == SYS_EBUSY);
    log_len = 0; log_text[0] = 0;
    int polls = 0;
    struct WlanScanResults* r = malloc(sizeof(*r));
    while (1) {
        athn_poll(); polls++;
        CHECK(wlan_scan_results(0, r) == SYS_OK);
        if (r->state != WLAN_SCAN_RUNNING) break;
        CHECK(polls < 20);
    }
    CHECK(polls == 13 && r->state == WLAN_SCAN_DONE && r->count == 3 && r->firmware == 1);
    CHECK(log_len == 0);                                        // Quiet: the desktop owns the screen
    CHECK(r->bss[0].rssi == 47 - 95 && memcmp(r->bss[0].bssid, (uint8_t[]){ 0x9c, 0x53, 0x22, 1, 2, 3 }, 6) == 0);
    CHECK(r->bss[0].channel == 6 && r->bss[0].security == WLAN_SEC_WEP);
    CHECK(r->bss[2].channel == 11 && r->bss[2].ssid_length == 8);
    CHECK(!fake.rx_dma);                                        // Asleep after the scan
    CHECK(wlan_info(0, &info) == SYS_OK && info.state == WLAN_STATE_RADIO_ON);
    CHECK(cd->ops->ioctl(cd, WLAN_IOCTL_SCAN_RESULTS, 0) == 0);
    CHECK(memcmp(user_mem, r, sizeof(*r)) == 0);

    // From the kernel console: says what happens
    cur_pid = PROCESS_INVALID_ID;
    CHECK(wlan_scan_start(0) == SYS_OK);
    for (int i = 0; i < 13; i++) athn_poll();
    printf("%s", log_text);
    CHECK(strstr(log_text, "athn: wlan0: channel 6: 30 frames, 30 beacons, 0 CRC/PHY errors, 0 bad") != NULL);
    CHECK(strstr(log_text, "athn: wlan0: scan done: 3 networks") != NULL);

    // The switch goes off: the scan fails and says why
    fake.switch_off = true;
    {
        // Only cards with the switch wired up check it
        struct AthnAdapter* a = adapters[0];
        a->chip.rfsilent = true; a->chip.rfsilent_pin = 3;
        CHECK(wlan_scan_start(0) == SYS_OK);
        athn_poll();
        CHECK(wlan_scan_results(0, r) == SYS_OK && r->state == WLAN_SCAN_FAILED);
        CHECK(wlan_info(0, &info) == SYS_OK && info.state == WLAN_STATE_RADIO_OFF);
        CHECK(strstr(log_text, "the wireless switch has the radio off") != NULL);
        fake.switch_off = false;
        a->chip.rfsilent = false;
    }

    // Remove mid-scan: stops DMA before freeing (vmm_free_pages checks), unregisters
    CHECK(wlan_scan_start(0) == SYS_OK);
    athn_poll(); athn_poll();
    device_remove(dev);
    CHECK(chardevs == 0 && netifs == 0 && pages_live == 0);
    CHECK(!(pci_cmd & PCI_COMMAND_BUS_MASTER));
    CHECK(wlan_info(0, &info) == SYS_ENOENT && wlan_scan_start(0) == SYS_ENOENT);
    athn_poll();
    printf("ok 5: PCI bind, /dev/wlan0, permissions, 13-channel scan, quiet vs console log, switch off, remove mid-scan\n");

    //
    // 6. Cards that don't come up
    //
    fake_reset(); rom_typical(1, 4096, false);
    map_fails = true;
    dev = device_add("pci3:00.0", NULL, &di);
    CHECK(dev != NULL && dev->driver == NULL && chardevs == 0);
    device_remove(dev);
    map_fails = false;

    fake_reset(); fake.srev = 0xFFFFFFFFU;
    dev = device_add("pci3:00.0", NULL, &di);
    CHECK(dev != NULL && dev->driver == NULL && chardevs == 0 && !(pci_cmd & PCI_COMMAND_BUS_MASTER));
    device_remove(dev);

    // No memory for receive pages: bound, but scanning says EIO
    fake_reset(); rom_typical(1, 4096, false);
    pages_fail_at_10 = true;
    dev = device_add("pci3:00.0", NULL, &di);
    CHECK(dev != NULL && dev->driver == &athn_pci_driver && pages_live == 0);
    CHECK(!(pci_cmd & PCI_COMMAND_BUS_MASTER));                 // Nothing for the card to write to
    CHECK(wlan_scan_start(0) == SYS_EIO);
    device_remove(dev);
    pages_fail_at_10 = false;
    CHECK(chardevs == 0 && pages_live == 0);
    printf("ok 6: unmappable BAR, dead card, no memory for buffers\n");

    //
    // 7. wlanN names are shared and reused
    //
    {
        static const struct WlanOps ops = { athn_wlan_info, athn_wlan_scan_start, athn_wlan_scan_results, NULL, NULL, NULL };
        struct WlanDev* w[WLAN_MAX_ADAPTERS + 1];
        for (int i = 0; i < WLAN_MAX_ADAPTERS; i++) {
            w[i] = wlan_register(&ops, NULL);
            CHECK(w[i] != NULL);
            char n[16]; snprintf(n, sizeof(n), "wlan%d", i);
            CHECK(strcmp(wlan_dev_name(w[i]), n) == 0);
        }
        CHECK(wlan_register(&ops, NULL) == NULL);
        wlan_unregister(w[2]);
        w[2] = wlan_register(&ops, NULL);
        CHECK(w[2] != NULL && strcmp(wlan_dev_name(w[2]), "wlan2") == 0);
        for (int i = 0; i < WLAN_MAX_ADAPTERS; i++) wlan_unregister(w[i]);
        CHECK(chardevs == 0);
        CHECK(wlan_info(9, &info) == SYS_ENOENT);
    }
    printf("ok 7: wlanN names shared, reused, bounded\n");

    //
    // 8. Transmitting (the chip): set-up, descriptors, statuses, power
    //
    fake_reset(); chip_init(); chip_tx_pages(); rom_typical(1, 4096, false);
    ap_reset("Test", NULL, 6);
    CHECK(ar9485_identify(&chip) && ar9485_attach(&chip, eep_image));
    CHECK(ar9485_tune(&chip, 6) && chip.tx_on);
    CHECK(fake.regs[AR_DQCUMASK(3) / 4] == 8 && fake.regs[AR_Q_DESC_CRCCHK / 4] == AR_Q_DESC_CRCCHK_EN);
    CHECK(fake.regs[AR_QMISC(0) / 4] == AR_Q_MISC_DCU_EARLY_TERM_REQ);
    CHECK(MS(fake.regs[AR_DLCL_IFS(0) / 4], AR_D_LCL_IFS_CWMIN) == 15 && MS(fake.regs[AR_DLCL_IFS(0) / 4], AR_D_LCL_IFS_AIFS) == 2);
    CHECK(fake.regs[AR_Q_STATUS_RING_START / 4] == chip.tx_status.phys);
    CHECK(fake.regs[AR_Q_STATUS_RING_END / 4] == chip.tx_status.phys + AR9485_TX_STATUS_COUNT * AR9485_TX_STATUS_SIZE);
    CHECK(fake.regs[AR_D_GBL_IFS_SIFS / 4] == 8 * 44 && fake.regs[AR_D_GBL_IFS_SLOT / 4] == 20 * 44);
    CHECK(MS(fake.regs[AR_TIME_OUT / 4], AR_TIME_OUT_ACK) == 64 * 44);
    CHECK(fake.regs[AR_STA_ID1 / 4] & AR_STA_ID1_PRESERVE_SEQNUM);
    // Transmit power: the ROM's, every OFDM 6-24 slot the same, open loop control on
    {
        uint32_t r1 = fake.regs[AR_PHY_PWRTX_RATE1 / 4];
        for (int k = 0; k < 4; k++) CHECK(((r1 >> (8 * k)) & 0x3f) == (uint32_t)chip.txpower[1]);
        for (int k = 0; k < 4; k++) CHECK(chip.txpower[k] >= 0 && chip.txpower[k] <= AR9485_TXPOWER_MAX);
        CHECK(chip.txpower[1] > 0);
        CHECK(MS(fake.regs[AR_PHY_TPC_6_B(0) / 4], AR_PHY_TPC_6_ERROR_EST_MODE) == 3);
        CHECK(fake.regs[AR_PHY_TX_FORCED_GAIN / 4] == 0);
        // A ROM asking for too much gets the ceiling; between samples, interpolated
        struct ar9380_eeprom* ee = (struct ar9380_eeprom*)eep_image;
        memset(ee->calTargetPower2G, 60, sizeof(ee->calTargetPower2G));
        CHECK(ar9485_tune(&chip, 6) && chip.txpower[1] == AR9485_TXPOWER_MAX);
        CHECK((fake.regs[AR_PHY_PWRTX_RATE2 / 4] & 0x3f) == AR9485_TXPOWER_MAX);
        ee->calTargetFbin2G[0] = 100; ee->calTargetFbin2G[1] = 120; ee->calTargetFbin2G[2] = 140;
        for (int k = 0; k < 4; k++) { ee->calTargetPower2G[0].tPow2x[k] = 20; ee->calTargetPower2G[1].tPow2x[k] = 30; ee->calTargetPower2G[2].tPow2x[k] = 34; }
        CHECK(ar9485_tune(&chip, 1) && chip.txpower[1] == 26);         // 2412 MHz: fbin 112, 20 + 12 * 10 / 20
        CHECK(ar9485_tune(&chip, 13) && chip.txpower[1] == 34);        // 2472: past the last sample
        // The correction is the ROM's signed value, masked into its field
        ee->calPierData2G[0][0].refPower = -3; ee->calPierData2G[0][1].refPower = -3; ee->calPierData2G[0][2].refPower = -3;
        CHECK(ar9485_tune(&chip, 6));
        CHECK(MS(fake.regs[AR_PHY_TPC_11_B(0) / 4], AR_PHY_TPC_11_OLPC_GAIN_DELTA) == 0xfd);
    }
    {
        uint8_t f[200]; memset(f, 0x33, sizeof(f));
        f[0] = 0x08; f[1] = 0x01; memcpy(f + 4, AP_MAC, 6); memcpy(f + 10, test_mac, 6); memcpy(f + 16, ROUTER, 6);
        static const uint8_t llc[8] = { 0xAA, 0xAA, 0x03, 0, 0, 0, 0x08, 0x00 };
        memcpy(f + 24, llc, 8);
        struct Ar9485TxRates rt = { { AR9485_RATE_OFDM24, AR9485_RATE_OFDM12, AR9485_RATE_CCK1, 0 }, { 2, 2, 4, 0 } };
        CHECK(ar9485_tx(&chip, f, 60, &rt) && chip.tx_queued == 1);
        fake_tx_run();
        struct Sent* sn = &sent[(sent_count - 1) % 64];
        CHECK(sn->len == 60 && memcmp(sn->frame, f, 60) == 0 && sn->acked);
        CHECK(MS(sn->ctl[14], AR_TXC14_XMIT_RATE0) == AR9485_RATE_OFDM24 && MS(sn->ctl[14], AR_TXC14_XMIT_RATE1) == AR9485_RATE_OFDM12);
        CHECK(MS(sn->ctl[14], AR_TXC14_XMIT_RATE2) == AR9485_RATE_CCK1);
        CHECK(MS(sn->ctl[13], AR_TXC13_XMIT_DATA_TRIES0) == 2 && MS(sn->ctl[13], AR_TXC13_XMIT_DATA_TRIES2) == 4);
        CHECK(MS(sn->ctl[13], AR_TXC13_XMIT_DATA_TRIES3) == 0 && (sn->ctl[13] & AR_TXC13_DUR_UPDATE_ENA));
        CHECK(MS(sn->ctl[15], AR_TXC15_PACKET_DUR0) == 44 && MS(sn->ctl[15], AR_TXC15_PACKET_DUR1) == 48);
        CHECK(MS(sn->ctl[16], AR_TXC16_PACKET_DUR2) == 314);
        CHECK(ar9485_tx_reclaim(&chip) == 1 && chip.tx_acked == 1 && chip.tx_queued == 0);
        CHECK(ar9485_tx_reclaim(&chip) == 0);

        // Nobody acknowledges: counted as failed
        fake.tx_deaf = true;
        CHECK(ar9485_tx(&chip, f, 60, &rt));
        fake_tx_run();
        CHECK(!sent[(sent_count - 1) % 64].acked && ar9485_tx_reclaim(&chip) == 1 && chip.tx_failed == 1);
        fake.tx_deaf = false;

        // To a group: unacknowledged, once, no duration
        uint8_t g[60]; memcpy(g, f, 60); memcpy(g + 4, BCAST, 6);
        CHECK(ar9485_tx(&chip, g, 60, &rt));
        fake_tx_run();
        sn = &sent[(sent_count - 1) % 64];
        CHECK((sn->ctl[12] & AR_TXC12_NO_ACK) && MS(sn->ctl[13], AR_TXC13_XMIT_DATA_TRIES0) == 1);
        CHECK(MS(sn->ctl[13], AR_TXC13_XMIT_DATA_TRIES1) == 0 && MS(sn->ctl[15], AR_TXC15_PACKET_DUR0) == 0);
        CHECK(ar9485_tx_reclaim(&chip) == 1 && chip.tx_acked == 2);

        // A card that never answers: eight in it, the ninth refused
        fake.tx_stall = true;
        for (int i = 0; i < AR9485_TX_COUNT; i++) CHECK(ar9485_tx(&chip, f, 60, &rt));
        CHECK(!ar9485_tx(&chip, f, 60, &rt) && strstr(chip.error, "every transmit buffer") != NULL);
        CHECK(ar9485_tx_reclaim(&chip) == 0 && chip.tx_queued == AR9485_TX_COUNT);
        fake.tx_stall = false;
        fake_tx_run();
        CHECK(ar9485_tx_reclaim(&chip) == AR9485_TX_COUNT && chip.tx_queued == 0);

        // Round the status ring a few times, buffers reused in order
        for (int i = 0; i < 100; i++) {
            f[30] = (uint8_t)i;
            CHECK(ar9485_tx(&chip, f, 40 + (uint32_t)i, &rt));
            if (i % 3 == 0) CHECK(ar9485_tx(&chip, f, 40, &rt));
            fake_tx_run();
            CHECK(sent[(sent_count - 1 - (i % 3 == 0)) % 64].frame[30] == (uint8_t)i);
            CHECK(ar9485_tx_reclaim(&chip) == (i % 3 == 0 ? 2u : 1u));
        }

        // Refused: too long, too short, an unknown rate, no tries
        uint8_t big[AR9485_TX_DATA_MAX + 1]; memset(big, 0, sizeof(big)); memcpy(big, f, 60);
        CHECK(!ar9485_tx(&chip, big, sizeof(big), &rt));
        CHECK(ar9485_tx(&chip, big, AR9485_TX_DATA_MAX, &rt));
        CHECK(!ar9485_tx(&chip, f, 9, &rt));
        struct Ar9485TxRates bad = { { 0x42, 0, 0, 0 }, { 2, 0, 0, 0 } };
        CHECK(!ar9485_tx(&chip, f, 60, &bad));
        struct Ar9485TxRates none = { { AR9485_RATE_CCK1, 0, 0, 0 }, { 0, 0, 0, 0 } };
        CHECK(!ar9485_tx(&chip, f, 60, &none));
        fake_tx_run();
        CHECK(ar9485_tx_reclaim(&chip) == 1);

        // Hostile statuses: random words, DONE set, in every slot. Never
        // more completions than frames queued, never past the ring.
        for (int it = 0; it < 2000; it++) {
            fake.tx_stall = true;
            uint32_t q = rnd() % 4;
            for (uint32_t i = 0; i < q; i++) CHECK(ar9485_tx(&chip, f, 60, &rt));
            for (int i = 0; i < AR9485_TX_STATUS_COUNT; i++) {
                uint32_t* st = (uint32_t*)(chip.tx_status.virt + i * AR9485_TX_STATUS_SIZE);
                for (int w = 0; w < 9; w++) st[w] = rnd();
                if (it & 1) st[0] = SM(AR_TXI_DESC_ID, AR_VENDOR_ATHEROS) | AR_TXI_DESC_TX | (rnd() & 0x0f00);
                if (it & 2) st[8] |= AR_TXS8_DONE;
            }
            uint8_t before = chip.tx_queued;
            uint32_t done = ar9485_tx_reclaim(&chip);
            CHECK(done <= before && chip.tx_queued == before - done && chip.tx_tail < AR9485_TX_COUNT);
            CHECK(chip.status_next < AR9485_TX_STATUS_COUNT);
            fake.tx_stall = false;
            CHECK(ar9485_tune(&chip, 6) && chip.tx_queued == 0);   // Starts the queue and ring afresh
            memset(chip.tx_status.virt, 0, 4096);
        }

        // A tune drops what's queued; stop stops the queue
        fake.tx_stall = true;
        for (int i = 0; i < 3; i++) CHECK(ar9485_tx(&chip, f, 60, &rt));
        CHECK(ar9485_tune(&chip, 6) && chip.tx_queued == 0 && fake.tx_head == fake.tx_tail);
        CHECK(ar9485_tx(&chip, f, 60, &rt));
        ar9485_stop(&chip);
        CHECK(!chip.tx_on && fake.tx_head == fake.tx_tail && !ar9485_tx(&chip, f, 60, &rt));
        CHECK(ar9485_tx_reclaim(&chip) == 0);
        fake.tx_stall = false;
    }
    // Receiving to us: the filter takes unicast once the BSS is set, and
    // the pad after a QoS header comes out again
    {
        CHECK(ar9485_tune(&chip, 6));
        CHECK(fake.regs[AR_RX_FILTER / 4] == (AR_RX_FILTER_BCAST | AR_RX_FILTER_BEACON));
        ar9485_set_bss(&chip, AP_MAC);
        CHECK(fake.regs[AR_RX_FILTER / 4] & AR_RX_FILTER_UCAST);
        CHECK(fake.regs[AR_BSS_ID0 / 4] == 0xDDCCBB0A && (fake.regs[AR_BSS_ID1 / 4] & 0xffff) == 0x01EE);
        uint8_t q[80]; memset(q, 0x44, sizeof(q));
        q[0] = 0x88; q[1] = 0x02; memcpy(q + 4, test_mac, 6); memcpy(q + 10, AP_MAC, 6); memcpy(q + 16, ROUTER, 6);
        air_to_sta(q, 70);
        fake_air_deliver();
        struct Got gq; memset(&gq, 0, sizeof(gq));
        CHECK(ar9485_rx_harvest(&chip, got_fn, &gq) == 1 && gq.f[0].length == 70 && memcmp(gq.copy[0], q, 70) == 0);
        // Too short to hold its pad: dropped
        uint32_t st[12]; uint8_t sh[40]; memcpy(sh, q, 40);
        make_status(st, 26 + 1 + 4, 40, AR_RXS11_FRAME_OK, 0);
        uint32_t bad0 = chip.rx_bad;
        fake_deliver_raw(false, st, sh, 31);
        CHECK(ar9485_rx_harvest(&chip, got_fn, &gq) == 0 && chip.rx_bad == bad0 + 1);
        ar9485_stop(&chip);
    }
    // Without transmit pages the chip still tunes, and never sends
    chip_init(); rom_typical(1, 4096, false);
    CHECK(ar9485_identify(&chip) && ar9485_attach(&chip, eep_image));
    CHECK(ar9485_tune(&chip, 6) && !chip.tx_on);
    { uint8_t f[40] = { 0x08 }; struct Ar9485TxRates rt = { { AR9485_RATE_CCK1 }, { 1 } }; CHECK(!ar9485_tx(&chip, f, 40, &rt)); }
    ar9485_stop(&chip);
    chip_free();
    CHECK(pages_live == 0);
    printf("ok 8: transmit: queue and timing set-up, ROM power (capped, interpolated), descriptors, acks, groups,\n"
           "     a stalled card, ring wrap, bad input, 2000 hostile status rings, tune/stop drop the queue, QoS pad\n");

    //
    // 9. Joining through the PCI driver: scan, WPA2, data both ways, a
    //    wrong password, an open 11b network, the access point gone, remove
    //
    {
        fake_reset(); rom_typical(1, 4096, false);
        air_count = 0; sent_count = 0;
        ap_reset("HomeNetwork", "correct horse battery", 6);
        dev = device_add("pci2:00.0", NULL, &di);
        CHECK(dev != NULL && dev->driver == &athn_pci_driver && pages_live == ATHN_PAGE_COUNT);
        cd = find_cd("wlan0");
        CHECK(cd != NULL && the_netif != NULL);
        cur_pid = 7; cur_sec = SYS_SEC_NET;
        struct AthnAdapter* a = adapters[0];

        #define LINK_AT 3000
        #define CONN_AT 1024
        struct WlanLink l;
        #define LINK() (l = t_link(cd))
        struct WlanConnect wc;
        #define SET_CONNECT(S, P) do { const char* pw_ = (P); memset(&wc, 0, sizeof(wc)); wc.version = WLAN_CONNECT_VERSION; \
            memcpy(wc.bssid, AP_MAC, 6); wc.ssid_length = (uint8_t)strlen(S); memcpy(wc.ssid, S, strlen(S)); \
            wc.passphrase_length = (uint8_t)(pw_ ? strlen(pw_) : 0); if (pw_) memcpy(wc.passphrase, pw_, strlen(pw_)); \
            memcpy(user_mem + CONN_AT, &wc, sizeof(wc)); } while (0)
        #define SCAN() do { CHECK(cd->ops->ioctl(cd, WLAN_IOCTL_SCAN, 0) == SYS_OK); \
            for (int k = 0; k < 13; k++) { athn_poll(); } \
            CHECK(wlan_scan_results(0, r) == SYS_OK && r->state == WLAN_SCAN_DONE); } while (0)

        // Never scanned: nothing to connect to
        SET_CONNECT("HomeNetwork", "correct horse battery");
        CHECK(cd->ops->ioctl(cd, WLAN_IOCTL_CONNECT, CONN_AT) == SYS_ENOENT);
        CHECK(LINK().state == WLAN_LINK_IDLE && l.reason == WLAN_REASON_NONE);
        SCAN();
        CHECK(r->count == 1 && r->bss[0].security == WLAN_SEC_WPA2 && r->bss[0].channel == 6);

        CHECK(cd->ops->ioctl(cd, WLAN_IOCTL_CONNECT, CONN_AT) == SYS_OK);
        CHECK(a->sta_on && fake.channel == 6);
        CHECK(cd->ops->ioctl(cd, WLAN_IOCTL_SCAN, 0) == SYS_EBUSY);            // The radio stays on its channel
        int polls = 0;
        while (LINK().state != WLAN_LINK_CONNECTED && polls < 100) { athn_poll(); polls++; }
        CHECK(l.state == WLAN_LINK_CONNECTED && ap.auths == 1 && ap.assocs == 1 && ap.m2s == 1 && ap.m4s == 1);
        CHECK(memcmp(l.bssid, AP_MAC, 6) == 0 && l.channel == 6);
        CHECK(the_netif->flags & NETIF_LINK);
        printf("   WPA2 join: %d polls (%d ms), %d frames sent\n", polls, polls * ATHN_STA_SLICE_MS, sent_count);
        // Management and the handshake at the lowest basic rate (1 Mbit/s)
        for (int i = 0; i < sent_count && i < 64; i++) {
            const struct Sent* sn = &sent[i];
            bool data = (sn->frame[0] & 0x0c) == 0x08;
            if (!data || !(sn->frame[1] & 0x40))
                CHECK(MS(sn->ctl[14], AR_TXC14_XMIT_RATE0) == AR9485_RATE_CCK1);
        }

        // The IP stack sends: encrypted, at 24 Mbit/s first
        uint8_t eth[64];
        memcpy(eth, ROUTER, 6); memcpy(eth + 6, test_mac, 6); eth[12] = 0x08; eth[13] = 0x00; memcpy(eth + 14, "hello", 5);
        CHECK(the_netif->ops->transmit(the_netif, eth, 19) == NET_OK);
        thread_sleep(1);
        CHECK(ap.data_in == 1 && ap.got_type == 0x0800 && ap.got_len == 5 && memcmp(ap.got, "hello", 5) == 0);
        const struct Sent* last = &sent[(sent_count - 1) % 64];
        CHECK((last->frame[1] & 0x40) && MS(last->ctl[14], AR_TXC14_XMIT_RATE0) == AR9485_RATE_OFDM24);
        CHECK(MS(last->ctl[14], AR_TXC14_XMIT_RATE3) == AR9485_RATE_CCK1 && MS(last->ctl[13], AR_TXC13_XMIT_DATA_TRIES3) == 4);

        // Encrypted QoS data in (the card pads it), and a group frame
        ap_data(test_mac, ROUTER, 0x0800, (const uint8_t*)"world", 5, true);
        athn_poll();
        CHECK(rx_eth_count == 1 && rx_eth_len == 19 && memcmp(rx_eth, test_mac, 6) == 0 && memcmp(rx_eth + 6, ROUTER, 6) == 0);
        CHECK(rx_eth[12] == 0x08 && rx_eth[13] == 0x00 && memcmp(rx_eth + 14, "world", 5) == 0);
        ap_data(BCAST, ROUTER, 0x0806, (const uint8_t*)"who-has", 7, true);
        athn_poll();
        CHECK(rx_eth_count == 2 && memcmp(rx_eth, BCAST, 6) == 0 && memcmp(rx_eth + 14, "who-has", 7) == 0);
        // Its address shows once DHCP gives one
        the_netif->ip = 0xC0A80105; the_netif->gateway = 0xC0A80101;
        CHECK(LINK().ip == 0xC0A80105 && l.gateway == 0xC0A80101);
        // A while later, still connected (beacons keep coming)
        for (int i = 0; i < 300; i++) athn_poll();
        CHECK(LINK().state == WLAN_LINK_CONNECTED && a->chip.tx_failed == 0);

        // Disconnect: a deauthentication goes out, link goes down, the radio stops
        int deauths = ap.deauths;
        CHECK(cd->ops->ioctl(cd, WLAN_IOCTL_DISCONNECT, 0) == SYS_OK);
        CHECK(ap.deauths == deauths + 1);                      // Sent before the queue stopped
        CHECK(!a->sta_on && !(the_netif->flags & NETIF_LINK) && !fake.rx_dma && !a->chip.tx_on);
        CHECK(LINK().state == WLAN_LINK_IDLE && l.reason == WLAN_REASON_USER);
        CHECK(the_netif->ops->transmit(the_netif, eth, 19) == NET_ERR_DOWN);

        // A wrong password: the handshake never finishes, and it says so
        SCAN();
        SET_CONNECT("HomeNetwork", "wrong password");
        int bad_before = ap.bad_mic;
        CHECK(cd->ops->ioctl(cd, WLAN_IOCTL_CONNECT, CONN_AT) == SYS_OK);
        polls = 0;
        while ((LINK().state != WLAN_LINK_IDLE) && polls < 400) { athn_poll(); polls++; }
        CHECK(l.state == WLAN_LINK_IDLE && l.reason == WLAN_REASON_PASSWORD && ap.bad_mic > bad_before);
        CHECK(!(the_netif->flags & NETIF_LINK) && !a->sta_on && !fake.rx_dma);

        // An open 802.11b network on channel 11: data in the clear, at 11 Mbit/s first
        ap_reset("CafeOpen", NULL, 11);
        ap.g_rates = false;
        SCAN();
        SET_CONNECT("CafeOpen", NULL);
        CHECK(cd->ops->ioctl(cd, WLAN_IOCTL_CONNECT, CONN_AT) == SYS_OK);
        polls = 0;
        while (LINK().state != WLAN_LINK_CONNECTED && polls < 100) { athn_poll(); polls++; }
        CHECK(l.state == WLAN_LINK_CONNECTED && l.security == 0 && (the_netif->flags & NETIF_LINK));
        CHECK(the_netif->ops->transmit(the_netif, eth, 19) == NET_OK);
        thread_sleep(1);
        last = &sent[(sent_count - 1) % 64];
        CHECK(ap.data_in == 1 && !(last->frame[1] & 0x40) && MS(last->ctl[14], AR_TXC14_XMIT_RATE0) == AR9485_RATE_CCK11);
        ap_data(test_mac, ROUTER, 0x0800, (const uint8_t*)"open", 4, false);
        athn_poll();
        CHECK(memcmp(rx_eth + 14, "open", 4) == 0);

        // The access point goes away: frames go unacknowledged, then its
        // beacons are missed and the connection ends
        ap.on_air = false;
        CHECK(the_netif->ops->transmit(the_netif, eth, 19) == NET_OK);
        polls = 0;
        while (LINK().state != WLAN_LINK_IDLE && polls < 400) { athn_poll(); polls++; }
        CHECK(l.reason == WLAN_REASON_LOST && a->chip.tx_failed >= 1 && !(the_netif->flags & NETIF_LINK));

        // Removed while connected: it leaves, link goes down, every page is freed
        ap.on_air = true;
        SCAN();
        CHECK(cd->ops->ioctl(cd, WLAN_IOCTL_CONNECT, CONN_AT) == SYS_OK);
        polls = 0;
        while (LINK().state != WLAN_LINK_CONNECTED && polls < 100) { athn_poll(); polls++; }
        CHECK(l.state == WLAN_LINK_CONNECTED);
        int changes = link_changes;
        deauths = ap.deauths;
        device_remove(dev);
        CHECK(link_changes == changes + 1 && ap.deauths == deauths + 1);
        CHECK(chardevs == 0 && netifs == 0 && pages_live == 0 && !(pci_cmd & PCI_COMMAND_BUS_MASTER));
        athn_poll();
        #undef LINK
        #undef SET_CONNECT
        #undef SCAN
    }
    printf("ok 9: WPA2 join over the fake card, data both ways (QoS padded, group), DHCP address in the link,\n"
           "     disconnect, wrong password, open 11b network, access point gone, removed while connected\n");

    free(r);
    printf("all passed (%ld checks)\n", checks);
    return 0;
}
