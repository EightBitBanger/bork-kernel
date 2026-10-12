#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/resources/resource_manager.h>
#include <kernel/vfs/vfs.h>
#include <kernel/util/string.h>

#if defined(KERNEL_PLATFORM_X86)
  #include <kernel/arch/x86/heap.h>
#elif defined(KERNEL_PLATFORM_AVR)
  #include <kernel/arch/avr/heap.h>
#else
  #error "resource_manager.c: define KERNEL_PLATFORM_X86 or KERNEL_PLATFORM_AVR"
#endif

#define RESOURCE_HEADER_SIZE    16u
#define RESOURCE_MAX_PAYLOAD    (16u * 1024u * 1024u)   // Sanity cap against corrupt headers
#define RESOURCE_IO_CHUNK       64u                     // Stack buffer for encoding (keep small)

#define SPRITE_HEADER_SIZE      6u
#define SPRITE_MAX_WORDS        ((RESOURCE_MAX_PAYLOAD - SPRITE_HEADER_SIZE) / 2u)

// ==========================================
// Little-endian encoding
// ==========================================

static inline void put_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static inline void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint16_t get_u16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ==========================================
// File I/O helpers (vfs_read/vfs_write may return short counts)
// ==========================================

static bool write_exact(File file, const void* buffer, uint32_t size) {
    const uint8_t* p = (const uint8_t*)buffer;
    while (size > 0) {
        int32_t n = vfs_write(file, p, size);
        if (n <= 0) return false;
        p += n;
        size -= (uint32_t)n;
    }
    return true;
}

static bool read_exact(File file, void* buffer, uint32_t size) {
    uint8_t* p = (uint8_t*)buffer;
    while (size > 0) {
        int32_t n = vfs_read(file, p, size);
        if (n <= 0) return false;
        p += n;
        size -= (uint32_t)n;
    }
    return true;
}

// ==========================================
// Sprite handler
// ==========================================

static uint32_t sprite_word_count(uint16_t width, uint16_t height, uint16_t palette_size) {
    // Max is exactly 0xFFFFFFFF, so this cannot overflow
    return (uint32_t)palette_size * 2u + (uint32_t)width * (uint32_t)height;
}

static uint32_t sprite_payload_size(const void* resource) {
    const struct Sprite* sprite = (const struct Sprite*)resource;
    uint32_t words = sprite_word_count(sprite->width, sprite->height, sprite->palette_size);
    if (words > SPRITE_MAX_WORDS) return 0;
    return SPRITE_HEADER_SIZE + words * 2u;
}

static bool sprite_write(File file, const void* resource) {
    const struct Sprite* sprite = (const struct Sprite*)resource;
    uint8_t buffer[RESOURCE_IO_CHUNK];
    
    put_u16(&buffer[0], sprite->width);
    put_u16(&buffer[2], sprite->height);
    put_u16(&buffer[4], sprite->palette_size);
    if (!write_exact(file, buffer, SPRITE_HEADER_SIZE)) return false;
    
    // Palette words then index words, encoded a chunk at a time
    uint32_t words = sprite_word_count(sprite->width, sprite->height, sprite->palette_size);
    uint32_t i = 0;
    while (i < words) {
        uint32_t n = 0;
        while (n + 2u <= sizeof(buffer) && i < words) {
            put_u16(&buffer[n], sprite->data[i]);
            n += 2u;
            i++;
        }
        if (!write_exact(file, buffer, n)) return false;
    }
    return true;
}

static void* sprite_read(File file, uint32_t payload_size) {
    if (payload_size < SPRITE_HEADER_SIZE) return NULL;
    
    uint8_t header[SPRITE_HEADER_SIZE];
    if (!read_exact(file, header, SPRITE_HEADER_SIZE)) return NULL;
    
    uint16_t width        = get_u16(&header[0]);
    uint16_t height       = get_u16(&header[2]);
    uint16_t palette_size = get_u16(&header[4]);
    
    if (width == 0 || height == 0 || palette_size == 0) return NULL;
    
    uint32_t words = sprite_word_count(width, height, palette_size);
    if (words > SPRITE_MAX_WORDS) return NULL;
    
    uint32_t data_bytes = words * 2u;
    if (payload_size != SPRITE_HEADER_SIZE + data_bytes) return NULL;
    
    uint32_t alloc_size = (uint32_t)sizeof(struct Sprite) + data_bytes;
#if SIZE_MAX < 0xFFFFFFFFu
    if (alloc_size > SIZE_MAX) return NULL;   // 16-bit size_t (AVR)
#endif
    
    struct Sprite* sprite = (struct Sprite*)malloc((size_t)alloc_size);
    if (sprite == NULL) return NULL;
    
    sprite->width        = width;
    sprite->height       = height;
    sprite->palette_size = palette_size;
    
    // data[] is declared const, but this is a heap object we own, so writing
    // through a cast is well defined.
    uint16_t* data = (uint16_t*)sprite->data;
    uint8_t*  raw  = (uint8_t*)data;
    
    if (!read_exact(file, raw, data_bytes)) {
        free(sprite);
        return NULL;
    }
    
    // Decode little-endian in place: word i occupies exactly bytes 2i and 2i+1,
    // which are read before being overwritten.
    for (uint32_t i = 0; i < words; i++) {
        data[i] = get_u16(&raw[i * 2u]);
    }
    
    return sprite;
}

static void sprite_release(void* resource) {
    free(resource);   // Header and data are a single allocation
}

// ==========================================
// Handler table
// ==========================================
//
// To add a resource type:
//   1. Append a value to ResourceType (never renumber existing ones)
//   2. Write payload_size / write / read / release for it
//   3. Add an entry below

typedef struct {
    const char* name;
    
    // Exact payload size in bytes, or 0 if the resource can't be stored
    uint32_t (*payload_size)(const void* resource);
    
    // Write the payload at the current file position
    bool (*write)(File file, const void* resource);
    
    // Read and validate payload_size bytes, returning a heap-allocated resource
    void* (*read)(File file, uint32_t payload_size);
    
    // Free a resource returned by read
    void (*release)(void* resource);
} ResourceHandler;

static const ResourceHandler handlers[RESOURCE_TYPE_COUNT] = {
    [RESOURCE_TYPE_SPRITE] = { "sprite", sprite_payload_size, sprite_write, sprite_read, sprite_release },
};

static const ResourceHandler* get_handler(ResourceType type) {
    if (type <= RESOURCE_TYPE_NONE || type >= RESOURCE_TYPE_COUNT) return NULL;
    const ResourceHandler* handler = &handlers[type];
    if (handler->payload_size == NULL || handler->write == NULL || handler->read == NULL) return NULL;
    return handler;
}

// ==========================================
// Header
// ==========================================

static bool read_header(File file, ResourceType* out_type, uint32_t* out_payload) {
    uint32_t file_size = vfs_get_size(file);
    if (file_size < RESOURCE_HEADER_SIZE) return false;
    
    uint8_t header[RESOURCE_HEADER_SIZE];
    vfs_seek(file, 0);
    if (!read_exact(file, header, RESOURCE_HEADER_SIZE)) return false;
    
    if (memcmp(header, RESOURCE_MAGIC, 4) != 0) return false;
    if (get_u16(&header[4]) != RESOURCE_FORMAT_VERSION) return false;
    
    uint32_t payload = get_u32(&header[8]);
    if (payload > RESOURCE_MAX_PAYLOAD) return false;
    if (file_size - RESOURCE_HEADER_SIZE < payload) return false;   // Truncated file
    
    *out_type    = (ResourceType)get_u16(&header[6]);
    *out_payload = payload;
    return true;
}

// ==========================================
// Public interface
// ==========================================

bool resource_save(const char* path, ResourceType type, const void* resource) {
    if (path == NULL || path[0] == '\0' || resource == NULL) return false;
    
    const ResourceHandler* handler = get_handler(type);
    if (handler == NULL) return false;
    
    uint32_t payload = handler->payload_size(resource);
    if (payload == 0 || payload > RESOURCE_MAX_PAYLOAD) return false;
    uint32_t total_size = RESOURCE_HEADER_SIZE + payload;
    
    // Make sure the file exists (CREATE only creates it when it is missing)
    File file = vfs_open(path, VFS_OPEN_CREATE | VFS_OPEN_READ | VFS_OPEN_WRITE);
    if (file == VFS_INVALID_FILE) return false;
    vfs_close(file);
    
    // Size the file before writing: writes don't reliably extend a file, and
    // this also drops stale bytes when overwriting a larger resource
    // (same approach as dwm_desktop_layout_save)
    if (!vfs_truncate(path, total_size)) return false;
    
    file = vfs_open(path, VFS_OPEN_READ | VFS_OPEN_WRITE);
    if (file == VFS_INVALID_FILE) return false;
    vfs_seek(file, 0);
    
    uint8_t header[RESOURCE_HEADER_SIZE];
    memcpy(header, RESOURCE_MAGIC, 4);
    put_u16(&header[4], RESOURCE_FORMAT_VERSION);
    put_u16(&header[6], (uint16_t)type);
    put_u32(&header[8], payload);
    put_u32(&header[12], 0);
    
    bool ok = write_exact(file, header, RESOURCE_HEADER_SIZE) &&
              handler->write(file, resource);
    
    uint32_t final_size = vfs_get_size(file);
    vfs_close(file);
    
    return ok && final_size == total_size;
}

void* resource_load(const char* path, ResourceType expected_type, ResourceType* out_type) {
    if (out_type) *out_type = RESOURCE_TYPE_NONE;
    if (path == NULL || path[0] == '\0') return NULL;
    
    File file = vfs_open(path, VFS_OPEN_READ);
    if (file == VFS_INVALID_FILE) return NULL;
    
    void* resource = NULL;
    ResourceType type = RESOURCE_TYPE_NONE;
    uint32_t payload = 0;
    
    if (read_header(file, &type, &payload) &&
        (expected_type == RESOURCE_TYPE_NONE || expected_type == type)) {
        const ResourceHandler* handler = get_handler(type);
        if (handler != NULL) {
            resource = handler->read(file, payload);
        }
    }
    
    vfs_close(file);
    
    if (resource != NULL && out_type) *out_type = type;
    return resource;
}

bool resource_peek_type(const char* path, ResourceType* out_type) {
    if (path == NULL || path[0] == '\0' || out_type == NULL) return false;
    
    File file = vfs_open(path, VFS_OPEN_READ);
    if (file == VFS_INVALID_FILE) return false;
    
    ResourceType type = RESOURCE_TYPE_NONE;
    uint32_t payload = 0;
    bool ok = read_header(file, &type, &payload);
    vfs_close(file);
    
    if (!ok) return false;
    *out_type = type;
    return true;
}

void resource_free(ResourceType type, void* resource) {
    if (resource == NULL) return;
    const ResourceHandler* handler = get_handler(type);
    if (handler != NULL && handler->release != NULL) {
        handler->release(resource);
    } else {
        free(resource);
    }
}

// Sprites

bool resource_sprite_save(const char* path, const struct Sprite* sprite) {
    return resource_save(path, RESOURCE_TYPE_SPRITE, sprite);
}

struct Sprite* resource_sprite_load(const char* path) {
    return (struct Sprite*)resource_load(path, RESOURCE_TYPE_SPRITE, NULL);
}
