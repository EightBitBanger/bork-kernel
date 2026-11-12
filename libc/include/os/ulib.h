#ifndef _OS_ULIB_H
#define _OS_ULIB_H

/*
 * User libraries: code a program loads at run time
 *
 * A user library is one block of code and data that a program reads from a
 * file and runs in its own memory, so several programs can use the same
 * code without each carrying a copy in its own file. It is the user-mode
 * cousin of a driver file (kernel/driver/kdrv.h): the same layout, but it
 * starts with "ulib" instead of "kdrv", and the kernel never runs it.
 *
 * There is no dynamic linker. A library imports nothing by name and exports
 * nothing by name:
 *
 *   - what it needs from the program (malloc and friends) comes in a
 *     ulib_host_t that the loader passes to its entry point
 *   - what it offers goes back as one table of function pointers, the
 *     library's exports, whose layout the library's own header defines
 *     (os/stblib.h is an example) and whose version is the header's `abi`
 *
 * The pure helpers a library needs (memcpy, strlen, floor, sqrt, ...) are
 * linked into it when it is built (ulib/runtime.c, src/math.c); libc.a is
 * not, since its heap, stdio and system calls belong to the program.
 *
 * Each program that opens a library gets its own copy in its own heap:
 * libraries share the file on disk and the code in the source tree, not
 * memory.
 *
 * Building one (programs/libc/ulib/build-ulib.sh does all three steps):
 *
 *   compile with the libc flags and -fno-pic
 *   link with programs/libc/ulib/ulib.ld and --emit-relocs: one image at 0
 *   pack with programs/libc/tools/mkulib: the image, its relocations and
 *   this header
 *
 * File layout, all numbers little-endian:
 *
 *   +----------------------+  0
 *   | ulib_header_t (64)   |
 *   +----------------------+  image_offset
 *   | image (image_size)   |  code, read-only data, data: linked at 0
 *   +----------------------+  reloc_offset (4-byte aligned)
 *   | uint32 reloc[count]  |  image offsets of 32-bit words holding an
 *   +----------------------+  address: the load address is added to each
 *
 * The entry point is
 *
 *   const void* ulib_entry(const ulib_host_t* host);
 *
 * and returns the exports table (inside the image), or NULL to refuse.
 *
 * This header is also compiled by tools/mkulib.c on the build machine, so
 * the format part uses nothing but <stdint.h>.
 */

#include <stdint.h>

#define ULIB_MAGIC          "ulib"
#define ULIB_MAGIC_SIZE     4
#define ULIB_VERSION        1           // ulib_header_t.version: this layout
#define ULIB_NAME_MAX       16          // ulib_header_t.name, terminator included
#define ULIB_MACHINE_I386   3           // ELF e_machine of the CPU the code is for

#define ULIB_FILE_MAX       (8u * 1024u * 1024u)    // Largest library file read
#define ULIB_IMAGE_MAX      (16u * 1024u * 1024u)   // image_size + bss_size

typedef struct {
    char     magic[ULIB_MAGIC_SIZE];    // "ulib"
    uint16_t version;                   // ULIB_VERSION
    uint16_t header_size;               // sizeof(ulib_header_t)
    uint16_t machine;                   // ULIB_MACHINE_*
    uint16_t reserved;                  // 0
    uint32_t abi;                       // Version of the library's exports table
    uint32_t host_version;              // Lowest ULIB_HOST_VERSION it needs
    uint32_t image_offset;              // File offset of the image
    uint32_t image_size;                // Bytes of image in the file
    uint32_t bss_size;                  // Zeroed bytes after the image
    uint32_t entry;                     // ulib_entry, as an image offset
    uint32_t reloc_offset;              // File offset of the relocation list
    uint32_t reloc_count;               // Entries in it
    uint32_t checksum;                  // CRC-32 of the whole file, this field taken as 0
    char     name[ULIB_NAME_MAX];       // The library's name
} ulib_header_t;

_Static_assert(sizeof(ulib_header_t) == 64, "ulib_header_t is a file format");

// CRC-32 (IEEE 802.3, reflected, as zip and PNG use). Start with crc = 0;
// feed the file in any number of pieces.
static inline uint32_t ulib_crc32(uint32_t crc, const void* data, uint32_t size) {
    const uint8_t* p = (const uint8_t*)data;
    crc = ~crc;
    for (uint32_t i = 0; i < size; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/*
 * What the program gives a library. Append-only: a library reads `size`
 * before using a member added after version 1, and its header says the
 * lowest ULIB_HOST_VERSION it needs.
 */
#define ULIB_HOST_VERSION   1

typedef struct {
    uint32_t size;                      // sizeof(ulib_host_t) of the program
    uint32_t version;                   // ULIB_HOST_VERSION of the program
    void*  (*malloc)(uint32_t size);
    void*  (*realloc)(void* p, uint32_t size);
    void   (*free)(void* p);
} ulib_host_t;

#ifndef ULIB_FORMAT_ONLY

typedef struct ulib ulib_t;

/*
 * Load the library file at `path` and run its entry point.
 *
 *   name   the name it must carry (NULL: any)
 *   abi    the exports version the caller was built for; the file's must
 *          be the same
 *
 * The file is checked before anything in it runs: magic, layout, CPU, CRC,
 * every relocation and the entry point inside the image. Returns the
 * library, or NULL with errno: ENOEXEC (not a library, damaged, built for
 * another CPU, another name or another abi, or its entry point refused),
 * EFBIG (over ULIB_FILE_MAX), ENOMEM, or whatever opening or reading the
 * file gave. ulib_error() says which.
 */
ulib_t* ulib_open(const char* path, const char* name, uint32_t abi);

// The table the library's entry point returned
const void* ulib_exports(const ulib_t* lib);

// Name and size in memory (image + bss)
const char* ulib_name(const ulib_t* lib);
uint32_t    ulib_size(const ulib_t* lib);

// Why the last ulib_open failed ("bad checksum", ...), "" when it didn't
const char* ulib_error(void);

// Free the library. Nothing it returned may be used afterwards: not its
// exports, not anything still pointing into it.
void ulib_close(ulib_t* lib);

#endif

#endif
