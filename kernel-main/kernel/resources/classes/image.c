#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/resources/classes/image.h>
#include <kernel/resources/resource_manager.h>
#include <kernel/resources/resource_class.h>
#include <kernel/resources/resource_io.h>
#include <kernel/vfs/vfs.h>

#ifdef KERNEL_PLATFORM_X86 
  #include <kernel/arch/x86/heap.h>
#endif

#define SPRITE_HEADER_SIZE      6u
#define SPRITE_IO_CHUNK         64u

// Same cap the manager puts on a payload
#define SPRITE_MAX_PAYLOAD      (16u * 1024u * 1024u)
#define SPRITE_MAX_WORDS        ((SPRITE_MAX_PAYLOAD - SPRITE_HEADER_SIZE) / 2u)

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
    uint8_t buffer[SPRITE_IO_CHUNK];

    resource_put_u16(&buffer[0], sprite->width);
    resource_put_u16(&buffer[2], sprite->height);
    resource_put_u16(&buffer[4], sprite->palette_size);
    if (!resource_write_exact(file, buffer, SPRITE_HEADER_SIZE)) return false;

    // Palette words then index words, encoded a chunk at a time
    uint32_t words = sprite_word_count(sprite->width, sprite->height, sprite->palette_size);
    uint32_t i = 0;
    while (i < words) {
        uint32_t n = 0;
        while (n + 2u <= sizeof(buffer) && i < words) {
            resource_put_u16(&buffer[n], sprite->data[i]);
            n += 2u;
            i++;
        }
        if (!resource_write_exact(file, buffer, n)) return false;
    }
    return true;
}

static void* sprite_read(File file, uint32_t payload_size) {
    if (payload_size < SPRITE_HEADER_SIZE) return NULL;

    uint8_t header[SPRITE_HEADER_SIZE];
    if (!resource_read_exact(file, header, SPRITE_HEADER_SIZE)) return NULL;

    uint16_t width        = resource_get_u16(&header[0]);
    uint16_t height       = resource_get_u16(&header[2]);
    uint16_t palette_size = resource_get_u16(&header[4]);

    if (width == 0 || height == 0 || palette_size == 0) return NULL;

    uint32_t words = sprite_word_count(width, height, palette_size);
    if (words > SPRITE_MAX_WORDS) return NULL;

    uint32_t data_bytes = words * 2u;
    if (payload_size != SPRITE_HEADER_SIZE + data_bytes) return NULL;

    uint32_t alloc_size = (uint32_t)sizeof(struct Sprite) + data_bytes;
#if SIZE_MAX < 0xFFFFFFFFu
    if (alloc_size > SIZE_MAX) return NULL;
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

    if (!resource_read_exact(file, raw, data_bytes)) {
        free(sprite);
        return NULL;
    }

    // Decode little-endian in place: word i occupies exactly bytes 2i and 2i+1,
    // which are read before being overwritten.
    for (uint32_t i = 0; i < words; i++) {
        data[i] = resource_get_u16(&raw[i * 2u]);
    }

    return sprite;
}

static void sprite_release(void* resource) {
    free(resource);   // Header and data are a single allocation
}

const ResourceHandler resource_class_image = {
    .name         = "sprite",
    .payload_size = sprite_payload_size,
    .write        = sprite_write,
    .read         = sprite_read,
    .release      = sprite_release,
};

// Typed wrappers

bool resource_sprite_save(const char* path, const struct Sprite* sprite) {
    return resource_save(path, RESOURCE_TYPE_SPRITE, sprite);
}

struct Sprite* resource_sprite_load(const char* path) {
    return (struct Sprite*)resource_load(path, RESOURCE_TYPE_SPRITE, NULL);
}
