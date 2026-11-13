#ifndef HDA_KSHIM_H
#define HDA_KSHIM_H

//
// The kernel calls the HDA driver used when it was built in, as calls
// through the driver table (kapi.h). Built in, hda.c and hda_codec.c called
// these functions directly; as a driver file they only have `kapi`, so the
// same names are kept here and passed on. Only for drivers/hda.
//

#include <kdrv_module.h>

extern const struct KernelApi* kapi;       // hda.c: set by kdrv_entry

// Kernel heap, time, console
#define malloc(n)               (kapi->malloc(n))
#define free(p)                 (kapi->free(p))
#define timer_get_ms()          (kapi->time_ms())
#define print(text)             (kapi->console_print(text))

// Locks: a pointer from kapi->mutex_create
typedef void* mutex_t;
#define mutex_lock(m)           (kapi->mutex_lock(*(m)))
#define mutex_unlock(m)         (kapi->mutex_unlock(*(m)))

// Character devices
#define chardev_register(n, o, d, s)    (kapi->chardev_register((n), (o), (d), (s)))
#define chardev_unregister(c)           (kapi->chardev_unregister(c))
#define chardev_copy_to_user(a, s, n)   (kapi->chardev_copy_to_user((a), (s), (n)))
#define chardev_copy_from_user(d, a, n) (kapi->chardev_copy_from_user((d), (a), (n)))

// PCI (arch/x86/bus/pci/pci.h)
#define PCI_COMMAND_MEMORY          (1U << 1)
#define PCI_COMMAND_BUS_MASTER      (1U << 2)
#define PCI_COMMAND_INTX_DISABLE    (1U << 10)
#define PCI_CLASS_MULTIMEDIA        0x04
#define PCI_SUBCLASS_HDA            0x03
#define PCI_COMMAND_OFFSET          0x04
#define pci_config_read8(b, s, f, o)        (kapi->pci_read8((b), (s), (f), (o)))
#define pci_config_read16(b, s, f, o)       (kapi->pci_read16((b), (s), (f), (o)))
#define pci_config_write8(b, s, f, o, v)    (kapi->pci_write8((b), (s), (f), (o), (v)))
#define pci_read_command(b, s, f)           (kapi->pci_read16((b), (s), (f), PCI_COMMAND_OFFSET))
#define pci_write_command(b, s, f, v)       (kapi->pci_write16((b), (s), (f), PCI_COMMAND_OFFSET, (v)))
#define pci_enable_command_bits(b, s, f, v) (kapi->pci_enable((b), (s), (f), (uint16_t)(v)))

// DMA pages (arch/x86/virtual/vmm.h)
#define PAGE_SIZE               4096u
#define vmm_alloc_pages(n)      (kapi->dma_alloc(n))
#define vmm_free_pages(p, n)    (kapi->dma_free((p), (n)))
#define vmm_get_phys_addr(p)    (kapi->dma_phys(p))

// Port 0x80: about a microsecond (arch/x86/io.h)
static inline void io_wait(void) {
    __asm__ volatile("outb %%al, $0x80" : : "a"(0));
}

// kstd.c: the string and number functions the kernel's util/ gave
size_t strlen(const char* s);
size_t strnlen(const char* s, size_t max);
char*  strncpy(char* dest, const char* src, size_t n);
char*  strrchr(const char* s, int c);
int    snprintf(char* out, size_t size, const char* format, ...)
       __attribute__((format(printf, 3, 4)));

#endif
