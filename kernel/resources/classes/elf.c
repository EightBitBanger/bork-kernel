#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/resources/classes/elf.h>
#include <kernel/resources/resource_io.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/vfs/vfs.h>
#include <kernel/malloc.h>
#include <kernel/util/string.h>

// Bytes moved per VFS read / user copy. One page: the copy goes through the
// VMM's scratch window one page at a time anyway.
#define ELF_COPY_CHUNK      PAGE_SIZE

#define PAGE_MASK           (~(PAGE_SIZE - 1))

// a + b, false if it wraps past 4 GB
static bool add_ok(uint32_t a, uint32_t b, uint32_t* out) {
    if (b > 0xFFFFFFFFU - a) return false;
    *out = a + b;
    return true;
}

static bool ident_ok(const Elf32_Ehdr* h) {
    return h->e_ident[EI_MAG0 + 0] == ELFMAG0 &&
           h->e_ident[EI_MAG0 + 1] == ELFMAG1 &&
           h->e_ident[EI_MAG0 + 2] == ELFMAG2 &&
           h->e_ident[EI_MAG0 + 3] == ELFMAG3;
}

bool elf_probe(const void* data, uint32_t size) {
    if (data == NULL || size < sizeof(Elf32_Ehdr)) return false;
    const Elf32_Ehdr* h = (const Elf32_Ehdr*)data;
    return ident_ok(h) &&
           h->e_ident[EI_CLASS] == ELFCLASS32 &&
           h->e_ident[EI_DATA]  == ELFDATA2LSB &&
           h->e_machine == EM_386 &&
           h->e_type == ET_EXEC;
}

static ElfResult check_header(const Elf32_Ehdr* h, uint32_t file_size, const char** why) {
    if (!ident_ok(h)) {
        *why = "not an ELF file";
        return ELF_ERR_NOT_ELF;
    }
    if (h->e_ident[EI_CLASS] != ELFCLASS32) {
        *why = "not a 32-bit ELF file";
        return ELF_ERR_UNSUPPORTED;
    }
    if (h->e_ident[EI_DATA] != ELFDATA2LSB) {
        *why = "not a little-endian ELF file";
        return ELF_ERR_UNSUPPORTED;
    }
    if (h->e_ident[EI_VERSION] != EV_CURRENT || h->e_version != EV_CURRENT) {
        *why = "unknown ELF version";
        return ELF_ERR_NOT_ELF;
    }
    if (h->e_ident[EI_OSABI] != ELFOSABI_SYSV && h->e_ident[EI_OSABI] != ELFOSABI_GNU) {
        *why = "built for another operating system ABI";
        return ELF_ERR_UNSUPPORTED;
    }
    if (h->e_machine != EM_386) {
        *why = "not an i386 executable";
        return ELF_ERR_UNSUPPORTED;
    }
    if (h->e_type == ET_DYN) {
        *why = "position independent (ET_DYN): link with -static -no-pie";
        return ELF_ERR_UNSUPPORTED;
    }
    if (h->e_type != ET_EXEC) {
        *why = "not an executable (object file or core dump)";
        return ELF_ERR_UNSUPPORTED;
    }
    if (h->e_ehsize < sizeof(Elf32_Ehdr)) {
        *why = "ELF header too small";
        return ELF_ERR_NOT_ELF;
    }
    if (h->e_phentsize != sizeof(Elf32_Phdr)) {
        *why = "unexpected program header size";
        return ELF_ERR_BAD_LAYOUT;
    }
    if (h->e_phnum == 0) {
        *why = "no program headers";
        return ELF_ERR_BAD_LAYOUT;
    }
    if (h->e_phnum == PN_XNUM || h->e_phnum > ELF_MAX_PHDRS) {
        *why = "too many program headers";
        return ELF_ERR_UNSUPPORTED;
    }

    uint32_t table_end;
    if (!add_ok(h->e_phoff, (uint32_t)h->e_phnum * sizeof(Elf32_Phdr), &table_end) ||
        table_end > file_size) {
        *why = "program header table runs past the end of the file";
        return ELF_ERR_BAD_LAYOUT;
    }
    return ELF_OK;
}

static inline bool is_load(const Elf32_Phdr* p) {
    return p->p_type == PT_LOAD && p->p_memsz != 0;
}

// Does any writable PT_LOAD segment touch the page at `page`?
static bool page_writable(const Elf32_Phdr* ph, uint32_t count, uint32_t page) {
    for (uint32_t i = 0; i < count; i++) {
        const Elf32_Phdr* p = &ph[i];
        if (!is_load(p) || !(p->p_flags & PF_W)) continue;

        // Validated: p_vaddr + p_memsz does not wrap
        uint32_t end = p->p_vaddr + p->p_memsz;
        if (p->p_vaddr < page + PAGE_SIZE && page < end) return true;
    }
    return false;
}

#define FAIL(code, text)  do { result = (code); info->error = (text); goto out; } while (0)

ElfResult elf_load(AddressSpace* space, File file, uint32_t file_size,
                   uint32_t region_start, uint32_t region_end, ElfImageInfo* info) {
    Elf32_Ehdr  eh;
    Elf32_Phdr* ph     = NULL;
    uint8_t*    buffer = NULL;
    ElfResult   result = ELF_OK;

    if (info == NULL) return ELF_ERR_IO;
    memset(info, 0, sizeof(*info));

    if (space == NULL || file == VFS_INVALID_FILE || region_end <= region_start)
        FAIL(ELF_ERR_IO, "invalid arguments");

    //
    // Header and program header table
    //

    if (file_size < sizeof(eh))
        FAIL(ELF_ERR_NOT_ELF, "file is smaller than an ELF header");
    if (!resource_read_at(file, 0, &eh, sizeof(eh)))
        FAIL(ELF_ERR_IO, "could not read the ELF header");

    result = check_header(&eh, file_size, &info->error);
    if (result != ELF_OK) goto out;

    uint32_t phnum = eh.e_phnum;
    ph = (Elf32_Phdr*)malloc(phnum * sizeof(Elf32_Phdr));
    if (ph == NULL)
        FAIL(ELF_ERR_NO_MEMORY, "out of memory for the program headers");
    if (!resource_read_at(file, eh.e_phoff, ph, phnum * sizeof(Elf32_Phdr)))
        FAIL(ELF_ERR_IO, "could not read the program headers");

    //
    // Pass 1: validate everything before mapping anything
    //

    uint32_t lowest   = 0xFFFFFFFFU;
    uint32_t highest  = 0;
    uint32_t loads    = 0;
    bool     entry_ok = false;

    for (uint32_t i = 0; i < phnum; i++) {
        const Elf32_Phdr* p = &ph[i];

        switch (p->p_type) {
            case PT_INTERP:
                FAIL(ELF_ERR_UNSUPPORTED, "needs a dynamic linker (PT_INTERP): link with -static");
            case PT_DYNAMIC:
                FAIL(ELF_ERR_UNSUPPORTED, "needs dynamic relocation (PT_DYNAMIC): link with -static -no-pie");
            case PT_TLS:
                FAIL(ELF_ERR_UNSUPPORTED, "uses thread-local storage (PT_TLS)");
            case PT_LOAD:
                break;
            default:
                continue;   // PT_NOTE, PT_PHDR, PT_GNU_STACK, ...: nothing to do
        }
        if (p->p_memsz == 0) continue;

        if (p->p_filesz > p->p_memsz)
            FAIL(ELF_ERR_BAD_LAYOUT, "segment file size exceeds its memory size");

        uint32_t file_end;
        if (!add_ok(p->p_offset, p->p_filesz, &file_end) || file_end > file_size)
            FAIL(ELF_ERR_BAD_LAYOUT, "segment runs past the end of the file");

        uint32_t mem_end;
        if (!add_ok(p->p_vaddr, p->p_memsz, &mem_end) ||
            p->p_vaddr < region_start || mem_end > region_end)
            FAIL(ELF_ERR_BAD_LAYOUT, "segment outside the user image area (link at 0x40000000)");

        for (uint32_t j = 0; j < i; j++) {
            const Elf32_Phdr* q = &ph[j];
            if (!is_load(q)) continue;
            if (p->p_vaddr < q->p_vaddr + q->p_memsz && q->p_vaddr < mem_end)
                FAIL(ELF_ERR_BAD_LAYOUT, "loadable segments overlap");
        }

        if (p->p_vaddr < lowest) lowest  = p->p_vaddr;
        if (mem_end > highest)   highest = mem_end;
        loads++;

        if ((p->p_flags & PF_X) && eh.e_entry >= p->p_vaddr && eh.e_entry < mem_end)
            entry_ok = true;
    }

    if (loads == 0)
        FAIL(ELF_ERR_BAD_LAYOUT, "no loadable segments");
    if (!entry_ok)
        FAIL(ELF_ERR_BAD_LAYOUT, "entry point is not inside an executable segment");

    //
    // Pass 2: map every page the segments touch, each page once. Fresh
    // pages come zeroed from the VMM, which also takes care of .bss.
    //

    for (uint32_t i = 0; i < phnum; i++) {
        const Elf32_Phdr* p = &ph[i];
        if (!is_load(p)) continue;

        uint32_t first = p->p_vaddr & PAGE_MASK;
        uint32_t last  = (p->p_vaddr + p->p_memsz - 1) & PAGE_MASK;

        for (uint32_t page = first; ; page += PAGE_SIZE) {
            if (vmm_user_get_phys(space, page) == 0) {
                uint32_t flags = page_writable(ph, phnum, page) ? VM_READWRITE : 0;
                if (!vmm_user_alloc(space, page, 1, flags))
                    FAIL(ELF_ERR_NO_MEMORY, "out of memory mapping the segments");
            }
            if (page == last) break;
        }
    }

    //
    // Pass 3: copy the file bytes in. vmm_user_copy_to writes through the
    // scratch window, so read-only pages can be filled too.
    //

    buffer = (uint8_t*)malloc(ELF_COPY_CHUNK);
    if (buffer == NULL)
        FAIL(ELF_ERR_NO_MEMORY, "out of memory for the copy buffer");

    for (uint32_t i = 0; i < phnum; i++) {
        const Elf32_Phdr* p = &ph[i];
        if (!is_load(p)) continue;

        for (uint32_t done = 0; done < p->p_filesz; ) {
            uint32_t chunk = p->p_filesz - done;
            if (chunk > ELF_COPY_CHUNK) chunk = ELF_COPY_CHUNK;

            if (!resource_read_at(file, p->p_offset + done, buffer, chunk))
                FAIL(ELF_ERR_IO, "could not read a segment");
            if (!vmm_user_copy_to(space, p->p_vaddr + done, buffer, chunk))
                FAIL(ELF_ERR_NO_MEMORY, "could not copy a segment into the process");

            done += chunk;
        }

        if (p->p_flags & PF_X) info->code_bytes += p->p_filesz;
        else                   info->data_bytes += p->p_filesz;
        info->bss_bytes += p->p_memsz - p->p_filesz;
    }

    info->entry         = eh.e_entry;
    info->image_start   = lowest & PAGE_MASK;
    info->image_end     = (highest + PAGE_SIZE - 1) & PAGE_MASK;
    info->segment_count = loads;

out:
    free(buffer);
    free(ph);
    return result;
}

const char* elf_result_string(ElfResult result) {
    switch (result) {
        case ELF_OK:              return "ok";
        case ELF_ERR_IO:          return "read error";
        case ELF_ERR_NOT_ELF:     return "not an ELF file";
        case ELF_ERR_UNSUPPORTED: return "unsupported ELF executable";
        case ELF_ERR_BAD_LAYOUT:  return "malformed ELF executable";
        case ELF_ERR_NO_MEMORY:   return "out of memory";
        default:                  return "unknown error";
    }
}
