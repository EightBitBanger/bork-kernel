#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/arch/x86/bus/pci/pci.h>
#include <kernel/arch/x86/drivers/audio/audio.h>
#include <kernel/arch/x86/drivers/audio/hda.h>
#include <kernel/arch/x86/drivers/audio/hda_regs.h>
#include <kernel/arch/x86/drivers/serial.h>
#include <kernel/arch/x86/io.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/driver/chardev.h>
#include <kernel/driver/device.h>
#include <kernel/driver/driver.h>

#include <kernel/console/print.h>
#include <kernel/malloc.h>
#include <kernel/mutex.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/syscall/syscall.h>
#include <kernel/util/snprint.h>
#include <kernel/util/string.h>
#include <kernel/util/timer.h>

//
// Intel High Definition Audio controllers, for the Realtek ALC269 codec
// (Acer Aspire 7250: AMD Hudson FCH, 1002:4383) and others like it
//
// What it does: resets the controller, talks to the codecs over the
// CORB / RIRB rings (or the immediate command registers when those don't
// work), picks the codec with an analog output (the ALC269 first: the
// Radeon's HDMI codec has none) and routes its speaker, headphone and line
// out pins to one DAC (hda_codec.c). Then one output stream, played from a
// ring of HDA_BUF_PAGES pages, one buffer descriptor each, as /dev/audioN
// (audio.h): programs write raw PCM into the ring and the controller plays
// it round and round.
//
// No interrupts: the controller's line stays off. The stream's position
// (LPIB) is read on every write and status call and every HDA_POLL_MS on
// the drivers thread; what has been played is zeroed behind it, so a
// program that stops writing gets silence, never the old ring again.
//
// Output only: no recording, no mixing, no resampling. One handle at a
// time.
//
// DMA memory is ordinary cached kernel pages. Most controllers snoop the
// CPU caches; for those that don't (the ATI SB4xx-SB8xx / Hudson ones need
// a bit set first, done below) every write into the ring and the CORB is
// also flushed with CLFLUSH, and RIRB lines are flushed before they're read.
//

#define HDA_MAX_CONTROLLERS 4
#define HDA_MMIO_SIZE       0x1000      // Global registers and 30 stream descriptors
#define HDA_BUF_PAGES       32          // Ring: 128 KB, 680 ms of 48 kHz 16-bit stereo
#define HDA_RING_BYTES      (HDA_BUF_PAGES * PAGE_SIZE)
#define HDA_POLL_MS         10
#define HDA_JACK_MS         250
#define HDA_VERB_TIMEOUT_MS 20
#define HDA_STREAM_TAG      1
#define HDA_DEFAULT_VOLUME  80

static const struct DeviceMatch hda_match[] = {
    { .bus = BUS_PCI, .vendor = MATCH_ANY, .product = MATCH_ANY,
      .class_code = PCI_CLASS_MULTIMEDIA, .subclass = PCI_SUBCLASS_HDA, .protocol = MATCH_ANY },
    { 0 }
};

struct HdaController {
    struct Device*     dev;
    struct CharDevice* cd;
    struct CharDevice* ctl;         // /dev/audioctlN: volume and status, any number of handles
    uint8_t  index;
    uint8_t  pci_bus, pci_dev, pci_func;
    uint16_t pci_vendor, pci_device;
    volatile uint8_t* regs;
    uint16_t gcap;
    uint8_t  vmaj, vmin;
    uint16_t codec_mask;
    uint32_t codec_ids[15];

    // Verbs
    bool      immediate;            // CORB / RIRB did not work: ICOI / ICII
    uint32_t* corb;
    uint32_t  corb_phys;
    uint32_t* rirb;
    uint32_t  rirb_phys;
    uint16_t  corb_entries, rirb_entries;
    uint16_t  corb_wp, rirb_rp;
    uint32_t  verb_failures;        // In a row; the link counts as dead past a few

    // The output stream
    uint8_t   sd;                   // Stream descriptor index (first output)
    uint32_t  sd_base;              // Its register offset
    uint16_t  fifo_bytes;
    struct HdaBdlEntry* bdl;
    uint32_t  bdl_phys;
    uint8_t*  page[HDA_BUF_PAGES];
    uint32_t  page_phys[HDA_BUF_PAGES];

    struct HdaCodec* codec;

    // State (hda_lock)
    bool      open;
    uint8_t   state;                // AUDIO_STATE_*
    uint32_t  rate;
    uint16_t  channels, format;     // AUDIO_FORMAT_*
    uint16_t  hda_format;           // SD_FMT
    uint16_t  frame_bytes;
    uint32_t  bytes_per_sec;
    uint32_t  start_bytes;
    uint8_t   vol_left, vol_right;
    bool      muted;

    uint64_t  play_pos;             // Ring bytes the controller has played since the last reset
    uint64_t  write_pos;            // Ring bytes filled since the last reset
    uint32_t  last_lpib;
    uint64_t  last_update_ms;
    uint64_t  dry_since_ms;         // Queue empty while running since then (0: it isn't)
    uint32_t  underruns;
    uint64_t  played_bytes;         // Since the format was set, silence included
    uint64_t  written_bytes;        // From the program, since the format was set
    uint64_t  last_jack_ms;
};

static mutex_t hda_lock;
static bool hda_ready = false;
static struct HdaController* controllers[HDA_MAX_CONTROLLERS];

//
// Kernel log: one line at boot on the screen, details on COM1
//

static void hda_serial(const char* text) {
    serial_puts("hda: ");
    serial_puts(text);
    serial_puts("\n");
}

//
// Registers (MMIO; offsets past the window read as all ones)
//

static uint32_t r32(struct HdaController* hc, uint32_t reg) {
    if (reg > HDA_MMIO_SIZE - 4) return 0xFFFFFFFFu;
    return *(volatile uint32_t*)(hc->regs + reg);
}
static uint16_t r16(struct HdaController* hc, uint32_t reg) {
    if (reg > HDA_MMIO_SIZE - 2) return 0xFFFF;
    return *(volatile uint16_t*)(hc->regs + reg);
}
static uint8_t r8(struct HdaController* hc, uint32_t reg) {
    if (reg > HDA_MMIO_SIZE - 1) return 0xFF;
    return *(volatile uint8_t*)(hc->regs + reg);
}
static void w32(struct HdaController* hc, uint32_t reg, uint32_t v) {
    if (reg <= HDA_MMIO_SIZE - 4) *(volatile uint32_t*)(hc->regs + reg) = v;
}
static void w16(struct HdaController* hc, uint32_t reg, uint16_t v) {
    if (reg <= HDA_MMIO_SIZE - 2) *(volatile uint16_t*)(hc->regs + reg) = v;
}
static void w8(struct HdaController* hc, uint32_t reg, uint8_t v) {
    if (reg <= HDA_MMIO_SIZE - 1) *(volatile uint8_t*)(hc->regs + reg) = v;
}

static void hda_delay_us(uint32_t us) {
    if (us >= 1000) {
        uint64_t start = timer_get_ms();
        uint32_t ms = (us + 999) / 1000;
        while (timer_get_ms() - start < ms + 1)
            io_wait();
        return;
    }
    for (uint32_t i = 0; i < us; i++)
        io_wait();
}

// Wait until (reg & mask) == want, at most timeout_ms. 8-bit or 32-bit.
static bool hda_wait8(struct HdaController* hc, uint32_t reg, uint8_t mask, uint8_t want, uint32_t timeout_ms) {
    uint64_t start = timer_get_ms();
    for (;;) {
        if ((r8(hc, reg) & mask) == want)
            return true;
        if (timer_get_ms() - start > timeout_ms)
            return false;
        io_wait();
    }
}

static bool hda_wait16(struct HdaController* hc, uint32_t reg, uint16_t mask, uint16_t want, uint32_t timeout_ms) {
    uint64_t start = timer_get_ms();
    for (;;) {
        if ((r16(hc, reg) & mask) == want)
            return true;
        if (timer_get_ms() - start > timeout_ms)
            return false;
        io_wait();
    }
}

static bool hda_wait32(struct HdaController* hc, uint32_t reg, uint32_t mask, uint32_t want, uint32_t timeout_ms) {
    uint64_t start = timer_get_ms();
    for (;;) {
        if ((r32(hc, reg) & mask) == want)
            return true;
        if (timer_get_ms() - start > timeout_ms)
            return false;
        io_wait();
    }
}

// Whether the CPU has CLFLUSH (CPUID 1, EDX bit 19). Every CPU this
// laptop class has does; an emulator's plain 32-bit CPU may not.
static int hda_clflush_ok = -1;

static bool hda_cpu_has_clflush(void) {
    if (hda_clflush_ok < 0) {
        uint32_t a = 1, b, c = 0, d;
        __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
        (void)b;
        hda_clflush_ok = (d & (1u << 19)) ? 1 : 0;
    }
    return hda_clflush_ok == 1;
}

// Write cache lines back to memory (and drop them), for a controller that
// does not snoop. Without CLFLUSH, snooping is all there is.
static void hda_flush(const void* p, uint32_t length) {
    if (!hda_cpu_has_clflush()) {
        __asm__ volatile("" : : : "memory");
        return;
    }
    uintptr_t a   = (uintptr_t)p & ~(uintptr_t)63;
    uintptr_t end = (uintptr_t)p + length;
    for (; a < end; a += 64)
        __asm__ volatile("clflush (%0)" : : "r"(a) : "memory");
    __asm__ volatile("mfence" : : : "memory");
}

//
// DMA pages
//

static void* hda_page(uint32_t* phys) {
    uint8_t* page = (uint8_t*)vmm_alloc_pages(1);
    if (page == NULL)
        return NULL;
    memset(page, 0, PAGE_SIZE);
    *phys = vmm_get_phys_addr(page);
    if (*phys == 0) {
        vmm_free_pages(page, 1);
        return NULL;
    }
    hda_flush(page, PAGE_SIZE);
    return page;
}

static void hda_free_pages(struct HdaController* hc) {
    if (hc->corb != NULL) vmm_free_pages(hc->corb, 1);
    if (hc->rirb != NULL) vmm_free_pages(hc->rirb, 1);
    if (hc->bdl != NULL)  vmm_free_pages(hc->bdl, 1);
    hc->corb = NULL;
    hc->rirb = NULL;
    hc->bdl  = NULL;
    for (uint32_t i = 0; i < HDA_BUF_PAGES; i++) {
        if (hc->page[i] != NULL)
            vmm_free_pages(hc->page[i], 1);
        hc->page[i] = NULL;
    }
}

static bool hda_alloc_pages(struct HdaController* hc) {
    hc->corb = (uint32_t*)hda_page(&hc->corb_phys);
    hc->rirb = (uint32_t*)hda_page(&hc->rirb_phys);
    hc->bdl  = (struct HdaBdlEntry*)hda_page(&hc->bdl_phys);
    bool ok = hc->corb != NULL && hc->rirb != NULL && hc->bdl != NULL;
    for (uint32_t i = 0; i < HDA_BUF_PAGES && ok; i++) {
        hc->page[i] = (uint8_t*)hda_page(&hc->page_phys[i]);
        ok = hc->page[i] != NULL;
    }
    if (!ok)
        hda_free_pages(hc);
    return ok;
}

//
// Codec commands
//

static bool hda_immediate_verb(struct HdaController* hc, uint32_t verb, uint32_t* response) {
    if (!hda_wait16(hc, HDA_ICIS, HDA_ICIS_BUSY, 0, HDA_VERB_TIMEOUT_MS))
        return false;
    w16(hc, HDA_ICIS, HDA_ICIS_VALID);              // Clear a stale answer (RW1C)
    w32(hc, HDA_ICOI, verb);
    w16(hc, HDA_ICIS, HDA_ICIS_BUSY);
    uint64_t start = timer_get_ms();
    for (;;) {
        uint16_t s = r16(hc, HDA_ICIS);
        if ((s & (HDA_ICIS_BUSY | HDA_ICIS_VALID)) == HDA_ICIS_VALID) {
            *response = r32(hc, HDA_ICII);
            w16(hc, HDA_ICIS, HDA_ICIS_VALID);
            return true;
        }
        if (timer_get_ms() - start > HDA_VERB_TIMEOUT_MS)
            return false;
        io_wait();
    }
}

static bool hda_corb_verb(struct HdaController* hc, uint32_t verb, uint32_t* response) {
    // Answers left over from a command that timed out: drop them
    uint16_t wp = r16(hc, HDA_RIRBWP) & 0xFF;
    hc->rirb_rp = (uint16_t)(wp % hc->rirb_entries);

    hc->corb_wp = (uint16_t)((hc->corb_wp + 1) % hc->corb_entries);
    hc->corb[hc->corb_wp] = verb;
    hda_flush(&hc->corb[hc->corb_wp], 4);
    w16(hc, HDA_CORBWP, hc->corb_wp);

    uint64_t start = timer_get_ms();
    for (;;) {
        wp = (uint16_t)((r16(hc, HDA_RIRBWP) & 0xFF) % hc->rirb_entries);
        while (hc->rirb_rp != wp) {
            hc->rirb_rp = (uint16_t)((hc->rirb_rp + 1) % hc->rirb_entries);
            volatile uint32_t* e = &hc->rirb[hc->rirb_rp * 2];
            hda_flush((const void*)e, 8);
            uint32_t lo = e[0], hi = e[1];
            if (hi & 0x10)
                continue;                           // Unsolicited (they are off)
            *response = lo;
            w8(hc, HDA_RIRBSTS, HDA_RIRBSTS_INTFL | HDA_RIRBSTS_OIS);
            return true;
        }
        if (timer_get_ms() - start > HDA_VERB_TIMEOUT_MS)
            return false;
        io_wait();
    }
}

// The one way to a codec (hda_verb_fn). After a few failures in a row
// CORB / RIRB gives way to the immediate registers; after more, the link
// counts as dead and every command fails at once.
static bool hda_verb(struct HdaController* hc, uint32_t verb, uint32_t* response) {
    if (hc->verb_failures >= 16)
        return false;
    bool ok;
    if (!hc->immediate) {
        ok = hda_corb_verb(hc, verb, response);
        if (!ok && ++hc->verb_failures >= 3) {
            hda_serial("CORB/RIRB not answering: immediate commands from now on");
            hc->immediate = true;
            hc->verb_failures = 0;
            ok = hda_immediate_verb(hc, verb, response);
        }
    } else {
        ok = hda_immediate_verb(hc, verb, response);
    }
    if (ok)
        hc->verb_failures = 0;
    else if (hc->immediate)
        hc->verb_failures++;
    return ok;
}

//
// Controller
//

static void hda_stream_stop_hw(struct HdaController* hc) {
    uint8_t ctl = r8(hc, hc->sd_base + HDA_SD_CTL);
    w8(hc, hc->sd_base + HDA_SD_CTL, (uint8_t)(ctl & ~(HDA_SD_CTL_RUN | 0x1C)));
    hda_wait8(hc, hc->sd_base + HDA_SD_CTL, HDA_SD_CTL_RUN, 0, 20);
    w8(hc, hc->sd_base + HDA_SD_STS, HDA_SD_STS_ALL);
}

// Everything that does DMA off: streams, CORB, RIRB
static void hda_dma_off(struct HdaController* hc) {
    uint32_t streams = HDA_GCAP_ISS(hc->gcap) + HDA_GCAP_OSS(hc->gcap) + HDA_GCAP_BSS(hc->gcap);
    for (uint32_t s = 0; s < streams && s < 30; s++) {
        uint32_t base = HDA_SD_BASE + s * HDA_SD_SIZE;
        uint8_t ctl = r8(hc, base + HDA_SD_CTL);
        w8(hc, base + HDA_SD_CTL, (uint8_t)(ctl & ~(HDA_SD_CTL_RUN | 0x1C)));
    }
    w8(hc, HDA_CORBCTL, 0);
    w8(hc, HDA_RIRBCTL, 0);
    hda_wait8(hc, HDA_CORBCTL, HDA_CORBCTL_RUN, 0, 20);
    hda_wait8(hc, HDA_RIRBCTL, HDA_RIRBCTL_DMAEN, 0, 20);
    w32(hc, HDA_DPLBASE, 0);
    w32(hc, HDA_INTCTL, 0);
}

static bool hda_reset(struct HdaController* hc) {
    hda_dma_off(hc);
    w16(hc, HDA_STATESTS, 0x7FFF);

    w32(hc, HDA_GCTL, r32(hc, HDA_GCTL) & ~HDA_GCTL_CRST);
    if (!hda_wait32(hc, HDA_GCTL, HDA_GCTL_CRST, 0, 100))
        return false;
    hda_delay_us(1000);
    w32(hc, HDA_GCTL, r32(hc, HDA_GCTL) | HDA_GCTL_CRST);
    if (!hda_wait32(hc, HDA_GCTL, HDA_GCTL_CRST, HDA_GCTL_CRST, 100))
        return false;

    // Codecs ask for an address within 25 frames of the reset; give them
    // well over that
    hda_delay_us(10000);
    hc->codec_mask = r16(hc, HDA_STATESTS) & 0x7FFF;
    w16(hc, HDA_STATESTS, 0x7FFF);
    // Unsolicited responses off: jacks are polled
    w32(hc, HDA_GCTL, r32(hc, HDA_GCTL) & ~HDA_GCTL_UNSOL);
    return true;
}

static uint16_t hda_ring_size(uint8_t reg, uint8_t* select) {
    if (reg & HDA_RINGSIZE_CAP_256) { *select = HDA_RINGSIZE_SEL_256; return 256; }
    if (reg & HDA_RINGSIZE_CAP_16)  { *select = HDA_RINGSIZE_SEL_16;  return 16; }
    *select = HDA_RINGSIZE_SEL_2;
    return 2;
}

static bool hda_rings_init(struct HdaController* hc) {
    uint8_t sel;

    // CORB
    w8(hc, HDA_CORBCTL, 0);
    hda_wait8(hc, HDA_CORBCTL, HDA_CORBCTL_RUN, 0, 20);
    w32(hc, HDA_CORBLBASE, hc->corb_phys);
    w32(hc, HDA_CORBUBASE, 0);
    hc->corb_entries = hda_ring_size(r8(hc, HDA_CORBSIZE), &sel);
    w8(hc, HDA_CORBSIZE, (uint8_t)((r8(hc, HDA_CORBSIZE) & ~3u) | sel));
    // Read pointer reset: set, see it set (not every controller shows it), clear
    w16(hc, HDA_CORBRP, HDA_CORBRP_RST);
    hda_wait16(hc, HDA_CORBRP, HDA_CORBRP_RST, HDA_CORBRP_RST, 5);
    w16(hc, HDA_CORBRP, 0);
    if (!hda_wait16(hc, HDA_CORBRP, HDA_CORBRP_RST, 0, 5))
        return false;
    w16(hc, HDA_CORBWP, 0);
    hc->corb_wp = 0;

    // RIRB
    w8(hc, HDA_RIRBCTL, 0);
    hda_wait8(hc, HDA_RIRBCTL, HDA_RIRBCTL_DMAEN, 0, 20);
    w32(hc, HDA_RIRBLBASE, hc->rirb_phys);
    w32(hc, HDA_RIRBUBASE, 0);
    hc->rirb_entries = hda_ring_size(r8(hc, HDA_RIRBSIZE), &sel);
    w8(hc, HDA_RIRBSIZE, (uint8_t)((r8(hc, HDA_RIRBSIZE) & ~3u) | sel));
    w16(hc, HDA_RIRBWP, HDA_RIRBWP_RST);
    w16(hc, HDA_RINTCNT, 1);
    hc->rirb_rp = 0;

    // RINTCTL: some controllers (QEMU's ICH6 among them) stop taking
    // commands after RINTCNT responses until RIRBSTS.INTFL is cleared, and
    // only set INTFL with RINTCTL on. INTCTL stays 0, so no interrupt
    // reaches the CPU; hda_corb_verb clears INTFL after every answer.
    w8(hc, HDA_RIRBSTS, HDA_RIRBSTS_INTFL | HDA_RIRBSTS_OIS);
    w8(hc, HDA_RIRBCTL, HDA_RIRBCTL_DMAEN | HDA_RIRBCTL_RINTCTL);
    w8(hc, HDA_CORBCTL, HDA_CORBCTL_RUN);
    return hda_wait8(hc, HDA_CORBCTL, HDA_CORBCTL_RUN, HDA_CORBCTL_RUN, 20);
}

// Controllers that need telling to snoop the CPU caches (Linux: azx_init_pci)
static void hda_pci_quirks(struct HdaController* hc) {
    uint16_t v = hc->pci_vendor, d = hc->pci_device;
    bool ati_sb = (v == 0x1002 && (d == 0x437B || d == 0x4383)) || (v == 0x1022 && d == 0x780D);
    if (ati_sb) {
        uint8_t b = pci_config_read8(hc->pci_bus, hc->pci_dev, hc->pci_func, 0x42);
        pci_config_write8(hc->pci_bus, hc->pci_dev, hc->pci_func, 0x42, (uint8_t)((b & ~0x07u) | 0x02));
    }
    if (v == 0x8086) {
        // TCSEL to TC0, as Linux does, against crackles on some ICHs
        uint8_t b = pci_config_read8(hc->pci_bus, hc->pci_dev, hc->pci_func, 0x44);
        pci_config_write8(hc->pci_bus, hc->pci_dev, hc->pci_func, 0x44, (uint8_t)(b & ~0x07u));
    }
}

//
// The output stream (hda_lock held)
//

static uint32_t hda_queued(const struct HdaController* hc) {
    return (hc->write_pos > hc->play_pos) ? (uint32_t)(hc->write_pos - hc->play_pos) : 0;
}

// Bytes kept between the controller's position and new data after the
// queue ran dry: it has read that far ahead into its FIFO already
static uint32_t hda_guard(const struct HdaController* hc) {
    uint32_t g = (uint32_t)hc->fifo_bytes + 64;
    return (g + hc->frame_bytes - 1) / hc->frame_bytes * hc->frame_bytes;
}

static void hda_ring_zero(struct HdaController* hc, uint64_t from, uint32_t length) {
    while (length > 0) {
        uint32_t off  = (uint32_t)(from % HDA_RING_BYTES);
        uint32_t pg   = off / PAGE_SIZE, in = off % PAGE_SIZE;
        uint32_t n    = PAGE_SIZE - in;
        if (n > length)
            n = length;
        memset(hc->page[pg] + in, 0, n);
        hda_flush(hc->page[pg] + in, n);
        from   += n;
        length -= n;
    }
}

static void hda_ring_copy(struct HdaController* hc, uint64_t to, const uint8_t* src, uint32_t length) {
    while (length > 0) {
        uint32_t off = (uint32_t)(to % HDA_RING_BYTES);
        uint32_t pg  = off / PAGE_SIZE, in = off % PAGE_SIZE;
        uint32_t n   = PAGE_SIZE - in;
        if (n > length)
            n = length;
        memcpy(hc->page[pg] + in, src, n);
        hda_flush(hc->page[pg] + in, n);
        to     += n;
        src    += n;
        length -= n;
    }
}

// Stream stopped, its descriptor reset, the ring silent and empty
static void hda_stream_reset(struct HdaController* hc) {
    hda_stream_stop_hw(hc);
    uint32_t base = hc->sd_base;
    w8(hc, base + HDA_SD_CTL, (uint8_t)(r8(hc, base + HDA_SD_CTL) | HDA_SD_CTL_SRST));
    hda_wait8(hc, base + HDA_SD_CTL, HDA_SD_CTL_SRST, HDA_SD_CTL_SRST, 20);
    w8(hc, base + HDA_SD_CTL, (uint8_t)(r8(hc, base + HDA_SD_CTL) & ~HDA_SD_CTL_SRST));
    hda_wait8(hc, base + HDA_SD_CTL, HDA_SD_CTL_SRST, 0, 20);

    hda_ring_zero(hc, 0, HDA_RING_BYTES);
    for (uint32_t i = 0; i < HDA_BUF_PAGES; i++) {
        hc->bdl[i].addr_lo = hc->page_phys[i];
        hc->bdl[i].addr_hi = 0;
        hc->bdl[i].length  = PAGE_SIZE;
        hc->bdl[i].flags   = 0;
    }
    hda_flush(hc->bdl, sizeof(struct HdaBdlEntry) * HDA_BUF_PAGES);

    w32(hc, base + HDA_SD_BDPL, hc->bdl_phys);
    w32(hc, base + HDA_SD_BDPU, 0);
    w32(hc, base + HDA_SD_CBL, HDA_RING_BYTES);
    w16(hc, base + HDA_SD_LVI, HDA_BUF_PAGES - 1);
    w16(hc, base + HDA_SD_FMT, hc->hda_format);
    // Stream tag in CTL bits 23:20, written as the whole register like
    // Linux does (not every controller takes a byte write at +2). The
    // status byte above it is RW1C: written as 0, it stays.
    uint32_t ctl = r32(hc, base + HDA_SD_CTL) & 0x00FFFFFFu;
    ctl = (ctl & ~0x00F00000u) | ((uint32_t)HDA_STREAM_TAG << 20);
    w32(hc, base + HDA_SD_CTL, ctl & ~(uint32_t)(HDA_SD_CTL_RUN | HDA_SD_CTL_SRST));
    w8(hc, base + HDA_SD_STS, HDA_SD_STS_ALL);
    hc->fifo_bytes = r16(hc, base + HDA_SD_FIFOS);
    if (hc->fifo_bytes > 4096)
        hc->fifo_bytes = 4096;

    hc->state        = AUDIO_STATE_STOPPED;
    hc->play_pos     = 0;
    hc->write_pos    = 0;
    hc->last_lpib    = 0;
    hc->dry_since_ms = 0;
}

static void hda_stream_run(struct HdaController* hc) {
    w8(hc, hc->sd_base + HDA_SD_STS, HDA_SD_STS_ALL);
    uint8_t ctl = r8(hc, hc->sd_base + HDA_SD_CTL);
    w8(hc, hc->sd_base + HDA_SD_CTL, (uint8_t)((ctl & ~0x1Cu) | HDA_SD_CTL_RUN));
    hc->state = AUDIO_STATE_RUNNING;
    hc->last_update_ms = timer_get_ms();
    hc->dry_since_ms = 0;
}

// Catch up with the controller: count and zero what it played, notice the
// queue running dry, stop after a second of nothing
static void hda_update(struct HdaController* hc) {
    if (hc->state != AUDIO_STATE_RUNNING)
        return;
    uint64_t now  = timer_get_ms();
    uint32_t lpib = r32(hc, hc->sd_base + HDA_SD_LPIB);
    if (lpib >= HDA_RING_BYTES)
        lpib = hc->last_lpib;                       // Nonsense: wait for the next one
    uint32_t delta = (lpib + HDA_RING_BYTES - hc->last_lpib) % HDA_RING_BYTES;

    // Away so long the controller may have gone round more than once
    // (a stalled drivers thread): the position can't be trusted to say
    // what is left, so treat it all as played
    uint64_t away = now - hc->last_update_ms;
    bool lost = hc->bytes_per_sec != 0 &&
                away * hc->bytes_per_sec / 1000 + hc->frame_bytes * 64 >= HDA_RING_BYTES;

    hc->last_lpib = lpib;
    hc->last_update_ms = now;
    uint32_t before = hda_queued(hc);
    if (lost) {
        hda_ring_zero(hc, 0, HDA_RING_BYTES);
        uint64_t round = (hc->play_pos - hc->play_pos % HDA_RING_BYTES) + HDA_RING_BYTES;
        uint64_t pos = round + lpib;
        hc->played_bytes += pos - hc->play_pos;
        hc->play_pos = pos;
        hc->write_pos = pos;                        // What was queued is gone
    } else if (delta > 0) {
        hda_ring_zero(hc, hc->play_pos, delta);
        hc->play_pos     += delta;
        hc->played_bytes += delta;
    }

    if (hc->write_pos < hc->play_pos)
        hc->write_pos = hc->play_pos;
    if (hda_queued(hc) == 0) {
        if (hc->dry_since_ms == 0) {
            hc->dry_since_ms = now;
            if (before > 0 || lost)
                hc->underruns++;
        } else if (now - hc->dry_since_ms >= AUDIO_IDLE_STOP_MS) {
            hda_stream_reset(hc);
        }
    } else {
        hc->dry_since_ms = 0;
    }
}

// Out of a pause. Controllers differ on where a stream picks up when RUN
// is set again (QEMU's starts its buffer list over), so the stream is
// reset and what was queued is moved to the start of the ring, where
// every controller begins.
static void hda_resume(struct HdaController* hc) {
    uint32_t queued = hda_queued(hc);
    uint8_t* keep = (queued > 0) ? (uint8_t*)malloc(queued) : NULL;
    if (queued > 0 && keep == NULL) {
        // No room to move it: carry on from where the controller says
        hc->last_lpib = r32(hc, hc->sd_base + HDA_SD_LPIB) % HDA_RING_BYTES;
        hda_stream_run(hc);
        return;
    }
    for (uint32_t done = 0; done < queued; ) {
        uint32_t off = (uint32_t)((hc->play_pos + done) % HDA_RING_BYTES);
        uint32_t pg  = off / PAGE_SIZE, in = off % PAGE_SIZE;
        uint32_t n   = PAGE_SIZE - in;
        if (n > queued - done)
            n = queued - done;
        memcpy(keep + done, hc->page[pg] + in, n);
        done += n;
    }
    hda_stream_reset(hc);
    if (queued > 0) {
        hda_ring_copy(hc, 0, keep, queued);
        hc->write_pos = queued;
        free(keep);
    }
    hda_stream_run(hc);
}

// Rate to the HDA format word's base / multiply / divide, and its
// AUDIO_RATE_* bit. False for rates HDA can't make.
static bool hda_rate(uint32_t rate, uint16_t* fmt, uint32_t* bit) {
    static const struct { uint32_t rate; uint16_t fmt; uint32_t bit; } t[] = {
        {   8000, HDA_FMT_DIV(6),                                  AUDIO_RATE_8000   },
        {  11025, HDA_FMT_BASE_44K | HDA_FMT_DIV(4),               AUDIO_RATE_11025  },
        {  16000, HDA_FMT_DIV(3),                                  AUDIO_RATE_16000  },
        {  22050, HDA_FMT_BASE_44K | HDA_FMT_DIV(2),               AUDIO_RATE_22050  },
        {  32000, HDA_FMT_MULT(2) | HDA_FMT_DIV(3),                AUDIO_RATE_32000  },
        {  44100, HDA_FMT_BASE_44K,                                AUDIO_RATE_44100  },
        {  48000, 0,                                               AUDIO_RATE_48000  },
        {  88200, HDA_FMT_BASE_44K | HDA_FMT_MULT(2),              AUDIO_RATE_88200  },
        {  96000, HDA_FMT_MULT(2),                                 AUDIO_RATE_96000  },
        { 176400, HDA_FMT_BASE_44K | HDA_FMT_MULT(4),              AUDIO_RATE_176400 },
        { 192000, HDA_FMT_MULT(4),                                 AUDIO_RATE_192000 },
    };
    for (uint32_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (t[i].rate == rate) {
            *fmt = t[i].fmt;
            *bit = t[i].bit;
            return true;
        }
    }
    return false;
}

static uint32_t hda_formats(const struct HdaCodec* c) {
    uint32_t f = 0;
    if (c->bits & 0x02) f |= AUDIO_FORMAT_S16;                       // 16-bit
    if (c->bits & 0x1C) f |= AUDIO_FORMAT_S32;                       // 20, 24 or 32 in 32-bit containers
    return f;
}

static int32_t hda_set_format(struct HdaController* hc, const struct AudioFormat* f) {
    uint16_t fmt;
    uint32_t bit;
    if (f->channels != 2 || !hda_rate(f->rate, &fmt, &bit) || !(hc->codec->rates & bit))
        return SYS_EINVAL;
    if (!(hda_formats(hc->codec) & f->format) || (f->format != AUDIO_FORMAT_S16 && f->format != AUDIO_FORMAT_S32))
        return SYS_EINVAL;

    uint16_t frame;
    if (f->format == AUDIO_FORMAT_S16) {
        fmt |= HDA_FMT_BITS_16;
        frame = 4;
    } else {
        // The deepest the codec takes; the samples are MSB-aligned either way
        uint32_t b = hc->codec->bits;
        fmt |= (b & 0x10) ? HDA_FMT_BITS_32 : (b & 0x08) ? HDA_FMT_BITS_24 : HDA_FMT_BITS_20;
        frame = 8;
    }
    fmt |= HDA_FMT_CHAN(2);

    uint32_t start = f->start_bytes;
    if (start == 0)
        start = HDA_RING_BYTES / 8;
    if (start > HDA_RING_BYTES)
        start = HDA_RING_BYTES;
    start = (start + frame - 1) / frame * frame;

    hc->rate          = f->rate;
    hc->channels      = 2;
    hc->format        = f->format;
    hc->hda_format    = fmt;
    hc->frame_bytes   = frame;
    hc->bytes_per_sec = f->rate * frame;
    hc->start_bytes   = start;
    hc->underruns     = 0;
    hc->played_bytes  = 0;
    hc->written_bytes = 0;

    hda_stream_reset(hc);
    hda_codec_stream(hc, hda_verb, hc->codec, HDA_STREAM_TAG, fmt);
    return SYS_OK;
}

static uint32_t hda_free_bytes(const struct HdaController* hc) {
    uint32_t q = hda_queued(hc);
    uint32_t free = HDA_RING_BYTES - q;
    // After running dry, new data goes a guard's length ahead
    if (hc->state == AUDIO_STATE_RUNNING && q == 0)
        free -= hda_guard(hc);
    return free / hc->frame_bytes * hc->frame_bytes;
}

//
// /dev/audioN
//

static int32_t hda_open(struct CharDevice* cd, uint16_t flags) {
    (void)flags;
    struct HdaController* hc = (struct HdaController*)cd->driver_data;
    mutex_lock(&hda_lock);
    int32_t r = hc->open ? SYS_EBUSY : SYS_OK;
    if (r == SYS_OK)
        hc->open = true;
    mutex_unlock(&hda_lock);
    return r;
}

static void hda_close(struct CharDevice* cd) {
    struct HdaController* hc = (struct HdaController*)cd->driver_data;
    mutex_lock(&hda_lock);
    // What was written plays out; the drivers thread stops the stream a
    // second after the queue runs dry
    hc->open = false;
    mutex_unlock(&hda_lock);
}

static int32_t hda_write(struct CharDevice* cd, const void* buffer, uint32_t size) {
    struct HdaController* hc = (struct HdaController*)cd->driver_data;
    mutex_lock(&hda_lock);
    hda_update(hc);
    if (hc->state == AUDIO_STATE_RUNNING && hda_queued(hc) == 0) {
        hc->write_pos = hc->play_pos + hda_guard(hc);
    }
    uint32_t n = hda_free_bytes(hc);
    if (n > size)
        n = size;
    n = n / hc->frame_bytes * hc->frame_bytes;
    if (n > 0) {
        hda_ring_copy(hc, hc->write_pos, (const uint8_t*)buffer, n);
        hc->write_pos     += n;
        hc->written_bytes += n;
        hc->dry_since_ms   = 0;
    }
    if (hc->state == AUDIO_STATE_STOPPED && hda_queued(hc) >= hc->start_bytes)
        hda_stream_run(hc);
    mutex_unlock(&hda_lock);
    return (int32_t)n;
}

static void hda_fill_info(struct HdaController* hc, struct AudioInfo* info) {
    memset(info, 0, sizeof(*info));
    info->version        = AUDIO_INFO_VERSION;
    info->codec_id       = hc->codec->vendor_id;
    info->codec_revision = hc->codec->revision;
    info->pci_vendor     = hc->pci_vendor;
    info->pci_device     = hc->pci_device;
    info->rates          = hc->codec->rates;
    info->formats        = hda_formats(hc->codec);
    info->channels_min   = 2;
    info->channels_max   = 2;
    info->outputs        = hc->codec->outputs;
    info->hw_volume      = hc->codec->has_volume ? 1 : 0;
    info->buffer_bytes   = HDA_RING_BYTES;
    strncpy(info->codec, hc->codec->name, sizeof(info->codec) - 1);
}

static void hda_fill_status(struct HdaController* hc, struct AudioStatus* s) {
    memset(s, 0, sizeof(*s));
    s->version        = AUDIO_STATUS_VERSION;
    s->state          = hc->state;
    s->headphone      = hc->codec->hp_present;
    s->frame_bytes    = hc->frame_bytes;
    s->queued_bytes   = hda_queued(hc);
    s->free_bytes     = hda_free_bytes(hc);
    s->buffer_bytes   = HDA_RING_BYTES;
    s->underruns      = hc->underruns;
    s->played_frames  = hc->played_bytes / hc->frame_bytes;
    s->written_frames = hc->written_bytes / hc->frame_bytes;
}

static void hda_fill_report(struct HdaController* hc, struct AudioReport* r) {
    memset(r, 0, sizeof(*r));
    r->version = AUDIO_REPORT_VERSION;
    char line[160];
    uint32_t used = 0;
    snprintf(line, sizeof(line),
             "controller %04x:%04x at %02x:%02x.%u, HDA %u.%u, streams %u in %u out %u bidi, %s, codecs %04x\n",
             (unsigned)hc->pci_vendor, (unsigned)hc->pci_device, (unsigned)hc->pci_bus,
             (unsigned)hc->pci_dev, (unsigned)hc->pci_func, (unsigned)hc->vmaj, (unsigned)hc->vmin,
             (unsigned)HDA_GCAP_ISS(hc->gcap), (unsigned)HDA_GCAP_OSS(hc->gcap), (unsigned)HDA_GCAP_BSS(hc->gcap),
             hc->immediate ? "immediate commands" : "CORB/RIRB", (unsigned)hc->codec_mask);
    for (uint32_t i = 0; line[i] != '\0' && used + 1 < sizeof(r->text); i++)
        r->text[used++] = line[i];
    for (uint32_t a = 0; a < 15; a++) {
        if (!(hc->codec_mask & (1u << a)))
            continue;
        snprintf(line, sizeof(line), "  link %u: %08x%s\n", (unsigned)a, (unsigned)hc->codec_ids[a],
                 a == hc->codec->addr ? " (used)" : "");
        for (uint32_t i = 0; line[i] != '\0' && used + 1 < sizeof(r->text); i++)
            r->text[used++] = line[i];
    }
    hda_codec_report(hc->codec, r->text, sizeof(r->text), &used);
    snprintf(line, sizeof(line), "stream %u fifo %u format %04x lpib %u sts %02x\n",
             (unsigned)hc->sd, (unsigned)hc->fifo_bytes, (unsigned)hc->hda_format,
             (unsigned)r32(hc, hc->sd_base + HDA_SD_LPIB), (unsigned)r8(hc, hc->sd_base + HDA_SD_STS));
    for (uint32_t i = 0; line[i] != '\0' && used + 1 < sizeof(r->text); i++)
        r->text[used++] = line[i];
    r->text[used] = '\0';
    r->length = used;
}

static int32_t hda_ioctl(struct CharDevice* cd, uint32_t request, uint32_t arg) {
    struct HdaController* hc = (struct HdaController*)cd->driver_data;
    int32_t r = SYS_OK;

    // The control device only looks and sets the volume: the stream
    // belongs to whoever holds /dev/audioN
    if (cd == hc->ctl && (request == AUDIO_IOCTL_SET_FORMAT || request == AUDIO_IOCTL_START ||
                          request == AUDIO_IOCTL_PAUSE || request == AUDIO_IOCTL_STOP))
        return SYS_EPERM;

    switch (request) {
        case AUDIO_IOCTL_INFO: {
            struct AudioInfo info;
            mutex_lock(&hda_lock);
            hda_fill_info(hc, &info);
            mutex_unlock(&hda_lock);
            return chardev_copy_to_user(arg, &info, sizeof(info)) ? SYS_OK : SYS_EFAULT;
        }
        case AUDIO_IOCTL_SET_FORMAT: {
            struct AudioFormat f;
            if (!chardev_copy_from_user(&f, arg, sizeof(f)))
                return SYS_EFAULT;
            if (f.version != AUDIO_FORMAT_VERSION)
                return SYS_EINVAL;
            mutex_lock(&hda_lock);
            r = hda_set_format(hc, &f);
            mutex_unlock(&hda_lock);
            return r;
        }
        case AUDIO_IOCTL_GET_FORMAT: {
            struct AudioFormat f;
            memset(&f, 0, sizeof(f));
            mutex_lock(&hda_lock);
            f.version     = AUDIO_FORMAT_VERSION;
            f.rate        = hc->rate;
            f.channels    = hc->channels;
            f.format      = hc->format;
            f.start_bytes = hc->start_bytes;
            mutex_unlock(&hda_lock);
            return chardev_copy_to_user(arg, &f, sizeof(f)) ? SYS_OK : SYS_EFAULT;
        }
        case AUDIO_IOCTL_START:
            mutex_lock(&hda_lock);
            if (hc->state == AUDIO_STATE_PAUSED)
                hda_resume(hc);
            else if (hc->state == AUDIO_STATE_STOPPED)
                hda_stream_run(hc);
            mutex_unlock(&hda_lock);
            return SYS_OK;
        case AUDIO_IOCTL_PAUSE:
            mutex_lock(&hda_lock);
            if (hc->state == AUDIO_STATE_RUNNING) {
                hda_update(hc);
                if (hc->state == AUDIO_STATE_RUNNING) {
                    hda_stream_stop_hw(hc);
                    hc->state = AUDIO_STATE_PAUSED;
                }
            }
            mutex_unlock(&hda_lock);
            return SYS_OK;
        case AUDIO_IOCTL_STOP:
            mutex_lock(&hda_lock);
            hda_update(hc);
            hda_stream_reset(hc);
            mutex_unlock(&hda_lock);
            return SYS_OK;
        case AUDIO_IOCTL_STATUS: {
            struct AudioStatus s;
            mutex_lock(&hda_lock);
            hda_update(hc);
            hda_fill_status(hc, &s);
            mutex_unlock(&hda_lock);
            return chardev_copy_to_user(arg, &s, sizeof(s)) ? SYS_OK : SYS_EFAULT;
        }
        case AUDIO_IOCTL_SET_VOLUME: {
            struct AudioVolume v;
            if (!chardev_copy_from_user(&v, arg, sizeof(v)))
                return SYS_EFAULT;
            if (v.version != AUDIO_VOLUME_VERSION || v.left > 100 || v.right > 100)
                return SYS_EINVAL;
            mutex_lock(&hda_lock);
            hc->vol_left  = v.left;
            hc->vol_right = v.right;
            hc->muted     = v.mute != 0;
            hda_codec_volume(hc, hda_verb, hc->codec, hc->vol_left, hc->vol_right, hc->muted);
            mutex_unlock(&hda_lock);
            return SYS_OK;
        }
        case AUDIO_IOCTL_GET_VOLUME: {
            struct AudioVolume v;
            memset(&v, 0, sizeof(v));
            mutex_lock(&hda_lock);
            v.version = AUDIO_VOLUME_VERSION;
            v.left    = hc->vol_left;
            v.right   = hc->vol_right;
            v.mute    = hc->muted ? 1 : 0;
            mutex_unlock(&hda_lock);
            return chardev_copy_to_user(arg, &v, sizeof(v)) ? SYS_OK : SYS_EFAULT;
        }
        case AUDIO_IOCTL_REPORT: {
            struct AudioReport* rep = (struct AudioReport*)malloc(sizeof(struct AudioReport));
            if (rep == NULL)
                return SYS_ENOMEM;
            mutex_lock(&hda_lock);
            hda_fill_report(hc, rep);
            mutex_unlock(&hda_lock);
            bool ok = chardev_copy_to_user(arg, rep, sizeof(*rep));
            free(rep);
            return ok ? SYS_OK : SYS_EFAULT;
        }
        default:
            return SYS_ENOTTY;
    }
}

// /dev/audioctlN reads as empty; read is there so it opens read-only
static int32_t hda_ctl_read(struct CharDevice* cd, void* buffer, uint32_t size) {
    (void)cd; (void)buffer; (void)size;
    return 0;
}

static const struct CharDeviceOps hda_ctl_ops = {
    .read  = hda_ctl_read,
    .ioctl = hda_ioctl,
};

static const struct CharDeviceOps hda_ops = {
    .open  = hda_open,
    .close = hda_close,
    .write = hda_write,
    .ioctl = hda_ioctl,
};

//
// Binding
//

// Stop all DMA, leave the controller in reset, bus mastering off
static void hda_quiesce(struct HdaController* hc) {
    if (hc->regs != NULL) {
        hda_dma_off(hc);
        w32(hc, HDA_GCTL, r32(hc, HDA_GCTL) & ~HDA_GCTL_CRST);
        hda_wait32(hc, HDA_GCTL, HDA_GCTL_CRST, 0, 100);
    }
    uint16_t cmd = pci_read_command(hc->pci_bus, hc->pci_dev, hc->pci_func);
    pci_write_command(hc->pci_bus, hc->pci_dev, hc->pci_func, (uint16_t)(cmd & ~PCI_COMMAND_BUS_MASTER));
}

static void hda_discard(struct HdaController* hc) {
    hda_quiesce(hc);
    hda_free_pages(hc);
    if (hc->codec != NULL)
        free(hc->codec);
    free(hc);
}

static int hda_probe(struct Device* dev) {
    if (!hda_ready) {
        mutex_init(&hda_lock);
        hda_ready = true;
    }
    const struct PCIDeviceInfo* pci = (const struct PCIDeviceInfo*)dev->bus_data;
    if (dev->bus != BUS_PCI || pci == NULL)
        return PROBE_NOT_MINE;

    struct HdaController* hc = (struct HdaController*)malloc(sizeof(struct HdaController));
    if (hc == NULL)
        return PROBE_FAILED;
    memset(hc, 0, sizeof(*hc));
    hc->dev        = dev;
    hc->pci_bus    = pci->bus;
    hc->pci_dev    = pci->dev;
    hc->pci_func   = pci->func;
    hc->pci_vendor = pci->vendor_id;
    hc->pci_device = pci->device_id;

    char line[128];
    char id[16];
    snprintf(id, sizeof(id), "%04x:%04x", (unsigned)hc->pci_vendor, (unsigned)hc->pci_device);

    pci_enable_command_bits(hc->pci_bus, hc->pci_dev, hc->pci_func,
                            PCI_COMMAND_MEMORY | PCI_COMMAND_INTX_DISABLE);
    hc->regs = (volatile uint8_t*)pci_map_bar(hc->pci_bus, hc->pci_dev, hc->pci_func, 0, HDA_MMIO_SIZE,
                                              VM_PRESENT | VM_READWRITE | VM_PCD);
    if (hc->regs == NULL) {
        snprintf(line, sizeof(line), "%s: BAR 0 could not be mapped", id);
        hda_serial(line);
        hda_discard(hc);
        return PROBE_FAILED;
    }
    hc->gcap = r16(hc, HDA_GCAP);
    hc->vmaj = r8(hc, HDA_VMAJ);
    hc->vmin = r8(hc, HDA_VMIN);
    if (hc->gcap == 0xFFFF || HDA_GCAP_OSS(hc->gcap) == 0) {
        snprintf(line, sizeof(line), "%s: no output streams (GCAP %04x)", id, (unsigned)hc->gcap);
        hda_serial(line);
        hda_discard(hc);
        return PROBE_NOT_MINE;
    }
    hc->sd      = (uint8_t)HDA_GCAP_ISS(hc->gcap);
    hc->sd_base = HDA_SD_BASE + (uint32_t)hc->sd * HDA_SD_SIZE;

    hda_pci_quirks(hc);
    if (!hda_alloc_pages(hc)) {
        snprintf(line, sizeof(line), "%s: out of memory", id);
        hda_serial(line);
        hda_discard(hc);
        return PROBE_FAILED;
    }
    if (!hda_reset(hc)) {
        snprintf(line, sizeof(line), "%s: controller would not leave reset", id);
        hda_serial(line);
        hda_discard(hc);
        return PROBE_FAILED;
    }
    pci_enable_command_bits(hc->pci_bus, hc->pci_dev, hc->pci_func, PCI_COMMAND_BUS_MASTER);
    if (!hda_rings_init(hc)) {
        hda_serial("CORB/RIRB would not start: immediate commands");
        hc->immediate = true;
        w8(hc, HDA_CORBCTL, 0);
        w8(hc, HDA_RIRBCTL, 0);
    }

    // The codec to use: the ALC269 if there is one, else the first with an
    // analog output
    hc->codec = (struct HdaCodec*)malloc(sizeof(struct HdaCodec));
    struct HdaCodec* tmp = (struct HdaCodec*)malloc(sizeof(struct HdaCodec));
    bool have = false;
    if (hc->codec != NULL && tmp != NULL) {
        for (uint8_t a = 0; a < 15; a++) {
            if (!(hc->codec_mask & (1u << a)))
                continue;
            hc->verb_failures = 0;
            uint32_t vendor = 0;
            if (!hda_verb(hc, HDA_VERB(a, 0, HDA_GET_PARAMETER, HDA_PAR_VENDOR_ID), &vendor))
                continue;
            hc->codec_ids[a] = vendor;
            if (have && hc->codec->vendor_id == 0x10EC0269u)
                continue;
            if (!hda_codec_parse(hc, hda_verb, tmp, a)) {
                snprintf(line, sizeof(line), "%s: codec %u (%08x) left alone: %s", id, (unsigned)a,
                         (unsigned)vendor, tmp->why != NULL ? tmp->why : "?");
                hda_serial(line);
                continue;
            }
            if (!have || tmp->vendor_id == 0x10EC0269u) {
                memcpy(hc->codec, tmp, sizeof(*tmp));
                have = true;
            }
        }
    }
    if (tmp != NULL)
        free(tmp);
    if (!have) {
        snprintf(line, sizeof(line), "%s: no codec with an analog output (codecs %04x)", id,
                 (unsigned)hc->codec_mask);
        hda_serial(line);
        hda_discard(hc);
        return PROBE_NOT_MINE;
    }
    hc->verb_failures = 0;
    hda_codec_setup(hc, hda_verb, hc->codec);
    hc->vol_left = hc->vol_right = HDA_DEFAULT_VOLUME;
    hda_codec_volume(hc, hda_verb, hc->codec, hc->vol_left, hc->vol_right, false);

    // 48 kHz 16-bit stereo, or the nearest the codec takes
    struct AudioFormat f = { AUDIO_FORMAT_VERSION, 48000, 2, AUDIO_FORMAT_S16, 0, 0 };
    if (!(hc->codec->rates & AUDIO_RATE_48000))
        f.rate = 44100;
    if (!(hda_formats(hc->codec) & AUDIO_FORMAT_S16))
        f.format = AUDIO_FORMAT_S32;
    if (hda_set_format(hc, &f) != SYS_OK) {
        snprintf(line, sizeof(line), "%s: %s takes neither 48 nor 44.1 kHz (rates %03x bits %02x)", id,
                 hc->codec->name, (unsigned)hc->codec->rates, (unsigned)hc->codec->bits);
        hda_serial(line);
        hda_discard(hc);
        return PROBE_FAILED;
    }

    mutex_lock(&hda_lock);
    int index = -1;
    for (int i = 0; i < HDA_MAX_CONTROLLERS && index < 0; i++) {
        if (controllers[i] == NULL)
            index = i;
    }
    mutex_unlock(&hda_lock);
    if (index < 0) {
        hda_serial("too many controllers");
        hda_discard(hc);
        return PROBE_FAILED;
    }
    hc->index = (uint8_t)index;

    char name[CHARDEV_NAME_MAX];
    snprintf(name, sizeof(name), "audio%d", index);
    hc->cd = chardev_register(name, &hda_ops, hc, 0);
    if (hc->cd == NULL) {
        snprintf(line, sizeof(line), "%s: /dev/%s could not be made", id, name);
        hda_serial(line);
        hda_discard(hc);
        return PROBE_FAILED;
    }
    // The control device: a volume tray icon keeps it open while programs
    // play through /dev/audioN. Missing it is not fatal.
    char ctl_name[CHARDEV_NAME_MAX];
    snprintf(ctl_name, sizeof(ctl_name), "audioctl%d", index);
    hc->ctl = chardev_register(ctl_name, &hda_ctl_ops, hc, 0);
    if (hc->ctl == NULL) {
        snprintf(line, sizeof(line), "%s: /dev/%s could not be made", id, ctl_name);
        hda_serial(line);
    }
    mutex_lock(&hda_lock);
    controllers[index] = hc;
    mutex_unlock(&hda_lock);

    // One boot line: "Realtek ALC269: speaker, headphone (audio0)"
    uint8_t o = hc->codec->outputs;
    snprintf(line, sizeof(line), "%s:%s%s%s%s%s (%s)\n", hc->codec->name,
             (o & AUDIO_OUT_SPEAKER) ? " speaker" : "",
             ((o & AUDIO_OUT_SPEAKER) && (o & (AUDIO_OUT_HEADPHONE | AUDIO_OUT_LINE))) ? "," : "",
             (o & AUDIO_OUT_HEADPHONE) ? " headphone" : "",
             ((o & AUDIO_OUT_HEADPHONE) && (o & AUDIO_OUT_LINE)) ? "," : "",
             (o & AUDIO_OUT_LINE) ? " line out" : "", name);
    print(line);
    snprintf(line, sizeof(line), "%s: %s on %s, %s", name, hc->codec->name, id,
             hc->immediate ? "immediate commands" : "CORB/RIRB");
    hda_serial(line);

    dev->driver_data = hc;
    return PROBE_BOUND;
}

static void hda_remove(struct Device* dev) {
    struct HdaController* hc = (struct HdaController*)dev->driver_data;
    if (hc == NULL)
        return;
    // Revokes open handles and waits out a call in progress
    if (hc->ctl != NULL)
        chardev_unregister(hc->ctl);
    chardev_unregister(hc->cd);
    mutex_lock(&hda_lock);
    controllers[hc->index] = NULL;
    mutex_unlock(&hda_lock);
    hda_discard(hc);
    dev->driver_data = NULL;
}

static void hda_poll(void) {
    if (!hda_ready)
        return;
    mutex_lock(&hda_lock);
    uint64_t now = timer_get_ms();
    for (int i = 0; i < HDA_MAX_CONTROLLERS; i++) {
        struct HdaController* hc = controllers[i];
        if (hc == NULL)
            continue;
        hda_update(hc);
        if (now - hc->last_jack_ms >= HDA_JACK_MS) {
            hc->last_jack_ms = now;
            hda_codec_jack(hc, hda_verb, hc->codec);
        }
    }
    mutex_unlock(&hda_lock);
}

static const struct Driver hda_driver = {
    .name    = "hda",
    .abi     = DRIVER_ABI,
    .flags   = DRIVER_BUILTIN,
    .version = "0.1",
    .match   = hda_match,
    .probe   = hda_probe,
    .remove  = hda_remove,
    .poll    = hda_poll,
    .poll_ms = HDA_POLL_MS,
};
DRIVER_BUILTIN_DECLARE(hda_driver);
