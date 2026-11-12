#ifndef _KERNEL_KDRV_H_
#define _KERNEL_KDRV_H_

// Loadable driver files
//
// A driver file is recognised by what it starts with, the four bytes
// "kdrv", never by its name: drivers live in sys/drivers/ with no file
// extension, like programs do. Built by tools/mkkdrv from a driver linked
// with drivers/kdrv.ld (see build-drivers.sh); loaded by
// driver/kdrv_loader.c (SYS_DRV_LOAD, the console's "load").
//
// Layout, all numbers little-endian:
//
//   +----------------------+  0
//   | KdrvHeader (64)      |
//   +----------------------+  image_offset
//   | image (image_size)   |  code, read-only data, data: one block linked at 0
//   +----------------------+  reloc_offset (4-byte aligned)
//   | uint32 reloc[count]  |  image offsets of 32-bit words that hold an
//   +----------------------+  address: the load address is added to each
//
// The loader copies the image into kernel pages, zeroes bss_size bytes after
// it, applies the relocations and calls the entry point:
//
//   const struct Driver* kdrv_entry(const struct KernelApi* api);
//
// which returns the module's driver (driver/driver.h) for the loader to
// register, or NULL to refuse. Everything the module may call in the kernel
// comes through `api` (driver/kapi.h): a module has no imports by name.
//
// This header is also compiled by tools/mkkdrv.c on the build machine, so it
// uses nothing but <stdint.h>.

#include <stdint.h>

#define KDRV_MAGIC          "kdrv"
#define KDRV_MAGIC_SIZE     4
#define KDRV_VERSION        1           // KdrvHeader.version: this layout
#define KDRV_NAME_MAX       16          // KdrvHeader.name, terminator included

// KdrvHeader.machine: the ELF e_machine value of the CPU the code is for
#define KDRV_MACHINE_I386   3

// Limits the loader holds a file to. A driver bigger than this is not
// "simple" any more.
#define KDRV_FILE_MAX       (1024u * 1024u)
#define KDRV_IMAGE_MAX      (1024u * 1024u)     // image_size + bss_size

typedef struct {
    char     magic[KDRV_MAGIC_SIZE];    // "kdrv"
    uint16_t version;                   // KDRV_VERSION
    uint16_t header_size;               // sizeof(KdrvHeader)
    uint16_t machine;                   // KDRV_MACHINE_*
    uint16_t driver_abi;                // DRIVER_ABI it was built against
    uint32_t kapi_version;              // Lowest KAPI_VERSION it needs
    uint32_t flags;                     // 0 (none defined yet)
    uint32_t image_offset;              // File offset of the image
    uint32_t image_size;                // Bytes of image in the file
    uint32_t bss_size;                  // Zeroed bytes after the image
    uint32_t entry;                     // kdrv_entry, as an image offset
    uint32_t reloc_offset;              // File offset of the relocation list
    uint32_t reloc_count;               // Entries in it
    uint32_t checksum;                  // CRC-32 of the whole file, this field taken as 0
    char     name[KDRV_NAME_MAX];       // The driver's name, for listings before loading
} KdrvHeader;

_Static_assert(sizeof(KdrvHeader) == 64, "KdrvHeader is a file format");

// CRC-32 (IEEE 802.3, reflected, as zip and PNG use), bit by bit: files are
// small and loaded rarely. Start with crc = 0; feed the file in any number
// of pieces.
static inline uint32_t kdrv_crc32(uint32_t crc, const void* data, uint32_t size) {
    const uint8_t* p = (const uint8_t*)data;
    crc = ~crc;
    for (uint32_t i = 0; i < size; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

#endif
