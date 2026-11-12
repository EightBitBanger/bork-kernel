/*
 * User libraries (os/ulib.h): load a "ulib" file into this program's heap,
 * relocate it and run its entry point.
 *
 * The image goes into ordinary heap memory and runs from there: the kernel
 * maps user pages without NX (see sys/mman.h), so that memory can hold
 * code. If the kernel ever enforces PROT_EXEC, this is the place that has
 * to ask for it.
 */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/ulib.h>

#define IMAGE_ALIGN     64

struct ulib {
    uint8_t*    image;                  // image_size + bss_size bytes, IMAGE_ALIGN aligned
    uint32_t    size;
    const void* exports;
    char        name[ULIB_NAME_MAX];
};

static const char* reason = "";

static void* host_malloc(uint32_t size)            { return malloc(size); }
static void* host_realloc(void* p, uint32_t size)  { return realloc(p, size); }
static void  host_free(void* p)                    { free(p); }

static const ulib_host_t host = {
    .size    = sizeof(ulib_host_t),
    .version = ULIB_HOST_VERSION,
    .malloc  = host_malloc,
    .realloc = host_realloc,
    .free    = host_free,
};

static void* fail(const char* why, int err) {
    reason = why;
    errno = err;
    return NULL;
}

// The whole file into a malloc'd buffer. NULL with errno and reason set.
static uint8_t* read_file(const char* path, uint32_t* size) {
    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return fail("can't open the file", errno != 0 ? errno : ENOENT);
    uint8_t* data = NULL;
    long length = -1;
    if (fseek(f, 0, SEEK_END) == 0)
        length = ftell(f);
    if (length < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fail("can't read the file", EIO);
    } else if ((unsigned long)length > ULIB_FILE_MAX) {
        fail("file larger than ULIB_FILE_MAX", EFBIG);
    } else if ((unsigned long)length < sizeof(ulib_header_t)) {
        fail("not a library (too short)", ENOEXEC);
    } else if ((data = (uint8_t*)malloc((size_t)length)) == NULL) {
        fail("out of memory", ENOMEM);
    } else if (fread(data, 1, (size_t)length, f) != (size_t)length) {
        free(data);
        data = NULL;
        fail("can't read the file", EIO);
    } else {
        *size = (uint32_t)length;
    }
    fclose(f);
    return data;
}

static uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// Is [offset, offset + length) inside [0, limit)?
static int inside(uint32_t offset, uint32_t length, uint32_t limit) {
    return offset <= limit && length <= limit - offset;
}

// Everything about the file that can be checked before it runs. The
// reason, or NULL when it is fine.
static const char* check(const uint8_t* file, uint32_t size, const ulib_header_t* h,
                         const char* name, uint32_t abi) {
    if (memcmp(h->magic, ULIB_MAGIC, ULIB_MAGIC_SIZE) != 0)
        return "not a library";
    if (h->version != ULIB_VERSION || h->header_size != sizeof(ulib_header_t))
        return "library format version not supported";
    if (h->machine != ULIB_MACHINE_I386)
        return "library built for another CPU";

    uint8_t copy[sizeof(ulib_header_t)];
    memcpy(copy, file, sizeof(copy));
    put_u32(copy + offsetof(ulib_header_t, checksum), 0);
    uint32_t crc = ulib_crc32(0, copy, sizeof(copy));
    crc = ulib_crc32(crc, file + sizeof(copy), size - (uint32_t)sizeof(copy));
    if (crc != h->checksum)
        return "bad checksum (damaged file)";

    if (h->name[ULIB_NAME_MAX - 1] != '\0' || h->name[0] == '\0')
        return "bad library name";
    if (name != NULL && strcmp(name, h->name) != 0)
        return "a different library than the one asked for";
    if (h->abi != abi)
        return "library version does not match the program's";
    if (h->host_version > ULIB_HOST_VERSION)
        return "library needs a newer libc";

    if (h->image_size == 0 || h->image_offset < sizeof(ulib_header_t) ||
        !inside(h->image_offset, h->image_size, size))
        return "image outside the file";
    if (h->bss_size > ULIB_IMAGE_MAX || h->image_size > ULIB_IMAGE_MAX - h->bss_size)
        return "image larger than ULIB_IMAGE_MAX";
    if (h->reloc_count > size / 4 || (h->reloc_offset & 3u) != 0 ||
        !inside(h->reloc_offset, h->reloc_count * 4u, size))
        return "relocation list outside the file";
    if (h->entry >= h->image_size)
        return "entry point outside the image";
    for (uint32_t i = 0; i < h->reloc_count; i++) {
        uint32_t at = get_u32(file + h->reloc_offset + i * 4u);
        if (h->image_size < 4 || at > h->image_size - 4)
            return "relocation outside the image";
    }
    return NULL;
}

ulib_t* ulib_open(const char* path, const char* name, uint32_t abi) {
    reason = "";
    if (path == NULL)
        return fail("no path", EINVAL);

    uint32_t size = 0;
    uint8_t* file = read_file(path, &size);
    if (file == NULL)
        return NULL;

    ulib_header_t h;
    memcpy(&h, file, sizeof(h));
    const char* bad = check(file, size, &h, name, abi);
    if (bad != NULL) {
        free(file);
        return fail(bad, ENOEXEC);
    }

    ulib_t* lib = (ulib_t*)calloc(1, sizeof(*lib));
    uint32_t total = h.image_size + h.bss_size;
    uint8_t* image = (uint8_t*)aligned_alloc(IMAGE_ALIGN, (total + IMAGE_ALIGN - 1) & ~(uint32_t)(IMAGE_ALIGN - 1));
    if (lib == NULL || image == NULL) {
        free(lib);
        free(image);
        free(file);
        return fail("out of memory", ENOMEM);
    }
    memcpy(image, file + h.image_offset, h.image_size);
    memset(image + h.image_size, 0, h.bss_size);
    uint32_t base = (uint32_t)(uintptr_t)image;
    for (uint32_t i = 0; i < h.reloc_count; i++) {
        uint8_t* word = image + get_u32(file + h.reloc_offset + i * 4u);
        put_u32(word, get_u32(word) + base);
    }
    free(file);

    typedef const void* (*entry_fn)(const ulib_host_t*);
    entry_fn entry = (entry_fn)(uintptr_t)(base + h.entry);
    const void* exports = entry(&host);
    if (exports == NULL || (uintptr_t)exports < base || (uintptr_t)exports >= base + total) {
        free(image);
        free(lib);
        return fail(exports == NULL ? "the library refused to start" : "exports outside the library", ENOEXEC);
    }

    lib->image   = image;
    lib->size    = total;
    lib->exports = exports;
    memcpy(lib->name, h.name, ULIB_NAME_MAX);
    return lib;
}

const void* ulib_exports(const ulib_t* lib) { return lib != NULL ? lib->exports : NULL; }
const char* ulib_name(const ulib_t* lib)    { return lib != NULL ? lib->name : ""; }
uint32_t    ulib_size(const ulib_t* lib)    { return lib != NULL ? lib->size : 0; }
const char* ulib_error(void)                { return reason; }

void ulib_close(ulib_t* lib) {
    if (lib == NULL)
        return;
    free(lib->image);
    free(lib);
}
