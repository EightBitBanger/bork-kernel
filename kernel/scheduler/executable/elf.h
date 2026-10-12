#ifndef _KERNEL_ELF_H_
#define _KERNEL_ELF_H_

// ELF32 executable loader (i386, static, non-PIE)
//
// elf_load() reads an executable through the VFS and builds its image in a
// user address space: every PT_LOAD segment gets zeroed pages at p_vaddr
// (so .bss needs no extra work), then p_filesz bytes are copied in from the
// file. Pages are read-only to ring 3 unless a writable segment (PF_W)
// touches them. Without PAE there is no NX bit, so PF_X is only used to
// check that the entry point lands in code.
//
// What is refused, and why:
//   ET_DYN / PT_DYNAMIC / PT_INTERP   needs relocation or a dynamic linker.
//                                     Link with -static -no-pie.
//   PT_TLS                            no thread pointer (%gs) set up yet
//   segments outside [region_start, region_end)
//                                     the default i386 link address is
//                                     0x08048000, which is kernel space here.
//                                     Link at 0x40000000 (user/link.ld).
//
// Segments may share a page (text tail + data head is common); each page is
// mapped once. Segments may not overlap byte for byte.

#include <stdint.h>
#include <stdbool.h>

#include <kernel/vfs/vfs.h>

//
// ELF32 on-disk structures (System V ABI)
//

typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Off;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Word;
typedef int32_t  Elf32_Sword;

#define EI_NIDENT       16

#define EI_MAG0         0
#define EI_CLASS        4
#define EI_DATA         5
#define EI_VERSION      6
#define EI_OSABI        7

#define ELFMAG0         0x7F
#define ELFMAG1         'E'
#define ELFMAG2         'L'
#define ELFMAG3         'F'

#define ELFCLASS32      1
#define ELFDATA2LSB     1
#define EV_CURRENT      1

#define ELFOSABI_SYSV   0
#define ELFOSABI_GNU    3

#define ET_REL          1
#define ET_EXEC         2
#define ET_DYN          3

#define EM_386          3

#define PN_XNUM         0xFFFF

#define PT_NULL         0
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_NOTE         4
#define PT_PHDR         6
#define PT_TLS          7

#define PF_X            0x1
#define PF_W            0x2
#define PF_R            0x4

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
} __attribute__((packed)) Elf32_Ehdr;

typedef struct {
    Elf32_Word    p_type;
    Elf32_Off     p_offset;
    Elf32_Addr    p_vaddr;
    Elf32_Addr    p_paddr;
    Elf32_Word    p_filesz;
    Elf32_Word    p_memsz;
    Elf32_Word    p_flags;
    Elf32_Word    p_align;
} __attribute__((packed)) Elf32_Phdr;

_Static_assert(sizeof(Elf32_Ehdr) == 52, "Elf32_Ehdr layout");
_Static_assert(sizeof(Elf32_Phdr) == 32, "Elf32_Phdr layout");

// Most program header tables have 4-10 entries
#define ELF_MAX_PHDRS   64

//
// Loader
//

typedef enum {
    ELF_OK = 0,
    ELF_ERR_IO,             // The file could not be read
    ELF_ERR_NOT_ELF,        // Bad magic / class / byte order / version
    ELF_ERR_UNSUPPORTED,    // Valid ELF this kernel can't run (PIE, dynamic, TLS, not i386)
    ELF_ERR_BAD_LAYOUT,     // Segments out of range, overlapping, past EOF, bad entry
    ELF_ERR_NO_MEMORY,      // Mapping the segments failed
} ElfResult;

typedef struct {
    uint32_t    entry;          // e_entry
    uint32_t    image_start;    // First mapped page (lowest segment, page aligned)
    uint32_t    image_end;      // End of the highest segment, rounded up to a page
    uint32_t    segment_count;  // PT_LOAD segments mapped
    uint32_t    code_bytes;     // File bytes in executable segments
    uint32_t    data_bytes;     // File bytes in the other segments
    uint32_t    bss_bytes;      // Zero-filled bytes (p_memsz - p_filesz, summed)
    const char* error;          // On failure: a static description, for logs
} ElfImageInfo;

struct AddressSpace;

// Load the ELF executable `file` (file_size bytes, open for reading) into
// `space`. Every segment must lie inside [region_start, region_end).
//
// On failure pages may already be mapped in `space`; the caller destroys the
// space (vmm_space_destroy frees them). Thread context only (VFS reads).
ElfResult elf_load(struct AddressSpace* space, File file, uint32_t file_size,
                   uint32_t region_start, uint32_t region_end, ElfImageInfo* info);

// True if `data` starts with an i386 ELF32 executable header (cheap probe)
bool elf_probe(const void* data, uint32_t size);

const char* elf_result_string(ElfResult result);

#endif
