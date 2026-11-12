//
// mkkdrv: turn a linked driver into a driver file
//
//   mkkdrv <driver.elf> <output> [name]
//
// The input is a 32-bit i386 ELF linked with drivers/kdrv.ld and
// --emit-relocs: one image at address 0 (code, read-only data, data, then
// bss) with the linker's relocations kept. mkkdrv copies the image out,
// lists every 32-bit absolute address in it (R_386_32) as a relocation and
// writes the header with the entry point (kdrv_entry), sizes and CRC-32.
//
// PC-relative references inside the image (R_386_PC32 / R_386_PLT32: calls
// and jumps) need nothing, since the image moves as a whole. Anything that
// needs a GOT means the driver was compiled as position independent code:
// build it with -fno-pic.
//
// name defaults to the output's file name. It must be the name in the
// driver's struct Driver; the kernel refuses a file whose two names differ.
//
// Runs on the build machine; needs nothing but a C99 compiler:
//   cc -O2 -I. -o build/tools/mkkdrv tools/mkkdrv.c
//

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "kernel/driver/kdrv.h"
#include "kernel/driver/kapi.h"       // DRIVER_ABI, KAPI_VERSION

typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} ElfHeader;

typedef struct {
    uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize;
} ElfSection;

typedef struct {
    uint32_t name, value, size;
    uint8_t  info, other;
    uint16_t shndx;
} ElfSymbol;

#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_NOBITS      8
#define SHT_REL         9
#define SHT_RELA        4
#define SHF_ALLOC       0x2
#define SHN_UNDEF       0
#define SHN_ABS         0xFFF1
#define ET_EXEC         2
#define EM_386          3

#define R_386_NONE      0
#define R_386_32        1
#define R_386_PC32      2
#define R_386_PLT32     4

static const char* input_name;

static void die(const char* message, const char* detail) {
    fprintf(stderr, "mkkdrv: %s: %s%s%s\n", input_name, message, detail ? ": " : "", detail ? detail : "");
    exit(1);
}

static uint8_t* read_all(const char* path, uint32_t* size) {
    FILE* f = fopen(path, "rb");
    if (f == NULL)
        die("can't open", NULL);
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (length <= 0 || length > 64L * 1024 * 1024)
        die("empty or too large", NULL);
    uint8_t* data = malloc((size_t)length);
    if (data == NULL || fread(data, 1, (size_t)length, f) != (size_t)length)
        die("can't read", NULL);
    fclose(f);
    *size = (uint32_t)length;
    return data;
}

static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: mkkdrv <driver.elf> <output> [name]\n");
        return 2;
    }
    input_name = argv[1];

    // Name: given, or the output's file name
    const char* name = argv[3];
    if (name == NULL) {
        name = strrchr(argv[2], '/');
        name = (name != NULL) ? name + 1 : argv[2];
    }
    if (name[0] == '\0' || strlen(name) >= KDRV_NAME_MAX)
        die("driver name empty or longer than 15 characters", name);

    uint32_t size;
    uint8_t* elf = read_all(argv[1], &size);

    ElfHeader eh;
    if (size < sizeof(eh))
        die("not an ELF file", NULL);
    memcpy(&eh, elf, sizeof(eh));
    if (memcmp(eh.ident, "\177ELF", 4) != 0 || eh.ident[4] != 1 || eh.ident[5] != 1)
        die("not a 32-bit little-endian ELF file", NULL);
    if (eh.machine != EM_386 || eh.type != ET_EXEC)
        die("not an i386 executable (link with drivers/kdrv.ld)", NULL);
    if (eh.shentsize != sizeof(ElfSection) || eh.shoff == 0 ||
        (uint64_t)eh.shoff + (uint64_t)eh.shnum * sizeof(ElfSection) > size || eh.shstrndx >= eh.shnum)
        die("bad section table", NULL);

    ElfSection* sections = calloc(eh.shnum, sizeof(ElfSection));
    memcpy(sections, elf + eh.shoff, eh.shnum * sizeof(ElfSection));
    for (uint32_t i = 0; i < eh.shnum; i++) {
        if (sections[i].type != SHT_NOBITS && (uint64_t)sections[i].offset + sections[i].size > size)
            die("section outside the file", NULL);
    }
    const char* shstr = (const char*)elf + sections[eh.shstrndx].offset;

    // Layout: data-carrying sections make the image, NOBITS after them the bss
    uint32_t image_size = 0, end = 0;
    for (uint32_t i = 0; i < eh.shnum; i++) {
        const ElfSection* s = &sections[i];
        if (!(s->flags & SHF_ALLOC) || s->size == 0)
            continue;
        if (s->type == SHT_NOBITS) {
            if (s->addr + s->size > end) end = s->addr + s->size;
        } else {
            if (s->addr + s->size > image_size) image_size = s->addr + s->size;
        }
    }
    if (end < image_size)
        end = image_size;
    for (uint32_t i = 0; i < eh.shnum; i++) {
        const ElfSection* s = &sections[i];
        if ((s->flags & SHF_ALLOC) && s->size != 0 && s->type == SHT_NOBITS && s->addr < image_size)
            die("bss before data: link with drivers/kdrv.ld", shstr + s->name);
    }
    if (image_size == 0)
        die("nothing to load", NULL);
    if (end > KDRV_IMAGE_MAX)
        die("image larger than KDRV_IMAGE_MAX", NULL);
    uint32_t bss_size = end - image_size;

    uint8_t* image = calloc(1, image_size);
    for (uint32_t i = 0; i < eh.shnum; i++) {
        const ElfSection* s = &sections[i];
        if ((s->flags & SHF_ALLOC) && s->type != SHT_NOBITS && s->size != 0)
            memcpy(image + s->addr, elf + s->offset, s->size);
    }

    // Symbols: the entry point, and what relocations refer to
    const ElfSection* symtab = NULL;
    for (uint32_t i = 0; i < eh.shnum; i++) {
        if (sections[i].type == SHT_SYMTAB)
            symtab = &sections[i];
    }
    if (symtab == NULL || symtab->entsize != sizeof(ElfSymbol))
        die("no symbol table (do not strip the .elf)", NULL);
    uint32_t symbol_count = symtab->size / sizeof(ElfSymbol);
    const char* strtab = (const char*)elf + sections[symtab->link].offset;

    int have_entry = 0;
    uint32_t entry = 0;
    for (uint32_t i = 0; i < symbol_count; i++) {
        ElfSymbol sym;
        memcpy(&sym, elf + symtab->offset + i * sizeof(ElfSymbol), sizeof(sym));
        if (strcmp(strtab + sym.name, "kdrv_entry") == 0 && sym.shndx != SHN_UNDEF) {
            entry = sym.value;
            have_entry = 1;
        }
    }
    if (!have_entry || entry >= image_size)
        die("no kdrv_entry function", NULL);

    // Relocations kept by --emit-relocs
    uint32_t reloc_cap = 64, reloc_count = 0;
    uint32_t* relocs = malloc(reloc_cap * sizeof(uint32_t));
    int saw_relocations = 0;
    for (uint32_t i = 0; i < eh.shnum; i++) {
        const ElfSection* s = &sections[i];
        if (s->type == SHT_RELA)
            die("RELA relocations are not expected on i386", shstr + s->name);
        if (s->type != SHT_REL || s->info >= eh.shnum || !(sections[s->info].flags & SHF_ALLOC))
            continue;
        saw_relocations = 1;
        for (uint32_t r = 0; r < s->size / 8; r++) {
            uint32_t offset, info;
            memcpy(&offset, elf + s->offset + r * 8, 4);
            memcpy(&info, elf + s->offset + r * 8 + 4, 4);
            uint32_t type = info & 0xFF, symbol = info >> 8;
            if (symbol >= symbol_count)
                die("relocation with a bad symbol", shstr + s->name);
            ElfSymbol sym;
            memcpy(&sym, elf + symtab->offset + symbol * sizeof(ElfSymbol), sizeof(sym));

            switch (type) {
                case R_386_NONE:
                    break;
                case R_386_32:
                    if (sym.shndx == SHN_ABS)
                        break;          // A constant, not an address in the image
                    if (offset > image_size - 4 || image_size < 4)
                        die("address relocation outside the image", shstr + s->name);
                    if (reloc_count == reloc_cap) {
                        reloc_cap *= 2;
                        relocs = realloc(relocs, reloc_cap * sizeof(uint32_t));
                    }
                    relocs[reloc_count++] = offset;
                    break;
                case R_386_PC32:
                case R_386_PLT32:
                    if (sym.shndx == SHN_ABS)
                        die("PC-relative reference to an absolute address", strtab + sym.name);
                    break;              // Inside the image: moves with it
                default: {
                    char text[64];
                    snprintf(text, sizeof(text), "relocation type %u (compile with -fno-pic)", (unsigned)type);
                    die("unsupported relocation", text);
                }
            }
        }
    }
    if (!saw_relocations)
        die("no relocations kept (link with --emit-relocs)", NULL);

    // Header, image, relocation list (4-byte aligned)
    uint32_t image_offset = sizeof(KdrvHeader);
    uint32_t reloc_offset = (image_offset + image_size + 3) & ~3u;
    uint32_t file_size = reloc_offset + reloc_count * 4;
    if (file_size > KDRV_FILE_MAX)
        die("driver file larger than KDRV_FILE_MAX", NULL);

    uint8_t* out = calloc(1, file_size);
    KdrvHeader h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, KDRV_MAGIC, KDRV_MAGIC_SIZE);
    h.version      = KDRV_VERSION;
    h.header_size  = sizeof(KdrvHeader);
    h.machine      = KDRV_MACHINE_I386;
    h.driver_abi   = DRIVER_ABI;
    h.kapi_version = KAPI_VERSION;
    h.image_offset = image_offset;
    h.image_size   = image_size;
    h.bss_size     = bss_size;
    h.entry        = entry;
    h.reloc_offset = reloc_offset;
    h.reloc_count  = reloc_count;
    strncpy(h.name, name, KDRV_NAME_MAX - 1);
    memcpy(out, &h, sizeof(h));
    memcpy(out + image_offset, image, image_size);
    for (uint32_t i = 0; i < reloc_count; i++)
        put_u32(out + reloc_offset + i * 4, relocs[i]);

    uint32_t crc = kdrv_crc32(0, out, file_size);
    put_u32(out + offsetof(KdrvHeader, checksum), crc);

    FILE* f = fopen(argv[2], "wb");
    if (f == NULL || fwrite(out, 1, file_size, f) != file_size || fclose(f) != 0) {
        fprintf(stderr, "mkkdrv: can't write %s\n", argv[2]);
        return 1;
    }
    printf("mkkdrv: %s: %s, %u bytes code+data, %u bss, %u relocations\n",
           argv[2], name, (unsigned)image_size, (unsigned)bss_size, (unsigned)reloc_count);
    return 0;
}
