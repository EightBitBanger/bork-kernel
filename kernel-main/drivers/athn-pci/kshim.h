#ifndef ATHN_KSHIM_H
#define ATHN_KSHIM_H

//
// The kernel as the AR9485 driver sees it. Built in, athn_pci.c and
// ar9485.c called the kernel's functions directly; as a driver file they
// only have `kapi` (driver/kapi.h), so the same names are kept here and
// passed on. Only for drivers/athn-pci.
//
// The host test (tests/athn) builds the same two files against its own
// fakes of those functions, by their kernel names: with ATHN_HOST_TEST set
// this header is the kernel's headers and nothing else.
//

#ifdef ATHN_HOST_TEST

#include <kernel/arch/x86/bus/pci/pci.h>
#include <kernel/arch/x86/io.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/driver/driver.h>
#include <kernel/driver/device.h>
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

static inline bool kshim_mutex_make(mutex_t* m) { mutex_init(m); return true; }
static inline void kshim_mutex_drop(mutex_t* m) { (void)m; }
static inline bool kshim_pci_location(const struct Device* dev, uint8_t* bus, uint8_t* slot, uint8_t* func) {
    const struct PCIDeviceInfo* pci = (const struct PCIDeviceInfo*)dev->bus_data;
    if (dev->bus != BUS_PCI || pci == NULL)
        return false;
    *bus = pci->bus; *slot = pci->dev; *func = pci->func;
    return true;
}
// Whether the call comes from the kernel console (no process)
static inline bool kshim_from_console(void) { return process_current_id() == PROCESS_INVALID_ID; }

#else

#include <kdrv_module.h>

extern const struct KernelApi* kapi;       // athn_pci.c: set by kdrv_entry

// Kernel heap, time, console
#define malloc(n)               (kapi->malloc(n))
#define free(p)                 (kapi->free(p))
#define timer_get_ms()          (kapi->time_ms())
#define thread_sleep(ms)        (kapi->sleep_ms(ms))
#define print(text)             (kapi->console_print(text))

// Locks: a pointer from kapi->mutex_create
typedef void* mutex_t;
#define mutex_lock(m)           (kapi->mutex_lock(*(m)))
#define mutex_unlock(m)         (kapi->mutex_unlock(*(m)))
static inline bool kshim_mutex_make(mutex_t* m) { *m = kapi->mutex_create(); return *m != NULL; }
static inline void kshim_mutex_drop(mutex_t* m) { if (*m != NULL) kapi->mutex_destroy(*m); *m = NULL; }

static inline bool kshim_from_console(void) { return kapi->caller_process(NULL, 0) == 0; }

// PCI (arch/x86/bus/pci/pci.h)
#define PCI_COMMAND_MEMORY          (1U << 1)
#define PCI_COMMAND_BUS_MASTER      (1U << 2)
#define PCI_COMMAND_INTX_DISABLE    (1U << 10)
#define PCI_COMMAND_OFFSET          0x04
#define kshim_pci_location(d, b, s, f)      (kapi->pci_location((d), (b), (s), (f)))
#define pci_read_command(b, s, f)           (kapi->pci_read16((b), (s), (f), PCI_COMMAND_OFFSET))
#define pci_write_command(b, s, f, v)       (kapi->pci_write16((b), (s), (f), PCI_COMMAND_OFFSET, (v)))
#define pci_enable_command_bits(b, s, f, v) (kapi->pci_enable((b), (s), (f), (uint16_t)(v)))
// The page flags the built-in driver asked for: registers, so uncached
#define VM_PRESENT              0x001u
#define VM_READWRITE            0x002u
#define VM_PCD                  0x010u
#define pci_map_bar(b, s, f, bar, bytes, flags) \
    (kapi->pci_map_bar((b), (s), (f), (bar), (bytes), ((flags) & VM_PCD) != 0))

// DMA pages (arch/x86/virtual/vmm.h)
#define PAGE_SIZE               4096u
#define vmm_alloc_pages(n)      (kapi->dma_alloc(n))
#define vmm_free_pages(p, n)    (kapi->dma_free((p), (n)))
#define vmm_get_phys_addr(p)    (kapi->dma_phys(p))

// The random pool (crypt/random.h)
#define crypt_random_add(d, n)      (kapi->random_add((d), (n)))
#define crypt_random_bytes(o, n)    (kapi->random_bytes((o), (n)))

// Port 0x80: about a microsecond (arch/x86/io.h)
static inline void io_wait(void) {
    __asm__ volatile("outb %%al, $0x80" : : "a"(0));
}

// kstd.c: what the kernel's util/ gave
char*  strncpy(char* dest, const char* src, size_t n);
int    snprintf(char* out, size_t size, const char* format, ...)
       __attribute__((format(printf, 3, 4)));

#endif

#endif
