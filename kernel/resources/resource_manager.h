#ifndef RESOURCE_MAN_H
#define RESOURCE_MAN_H

#include <stdint.h>
#include <stdbool.h>

#include <kernel/dwm/rendering/sprite.h>

/*
 * Binary resource files.
 *
 * Every resource file starts with the same 16-byte header, followed by a
 * type-specific payload. All multi-byte fields are little-endian and there
 * is no padding anywhere, so the files are identical on x86 and AVR.
 *
 *   Resource header (16 bytes)
 *     0   char[4]  magic          "RSRC"
 *     4   u16      version        RESOURCE_FORMAT_VERSION
 *     6   u16      type           ResourceType
 *     8   u32      payload_size   number of bytes following the header
 *    12   u32      reserved       0
 *
 *   Sprite payload (RESOURCE_TYPE_SPRITE), same order as struct Sprite in ui.c
 *     0   u16      width
 *     2   u16      height
 *     4   u16      palette_size
 *     6   u16[]    data: palette_size * 2 words of palette (each COLOR() is
 *                  two words), followed by width * height palette indices
 *
 *   payload_size = 6 + 2 * (palette_size * 2 + width * height)
 *
 */

#define RESOURCE_MAGIC              "RSRC"
#define RESOURCE_FORMAT_VERSION     1

// Values are written to disk: never renumber, only append.
typedef enum {
    RESOURCE_TYPE_NONE   = 0,   // Invalid / "any type" when loading
    RESOURCE_TYPE_SPRITE = 1,
    
    // New resource types go here
    
    RESOURCE_TYPE_COUNT
} ResourceType;

// Generic interface

// Serialize a resource of the given type to path (created or overwritten)
bool resource_save(const char* path, ResourceType type, const void* resource);

// Load a resource from path. Pass RESOURCE_TYPE_NONE as expected_type to accept
// any type; out_type (may be NULL) receives the type that was loaded.
// The result is heap allocated: release it with resource_free.
void* resource_load(const char* path, ResourceType expected_type, ResourceType* out_type);

// Read only the header of a resource file to find out what it holds
bool resource_peek_type(const char* path, ResourceType* out_type);

// Release a resource returned by resource_load
void resource_free(ResourceType type, void* resource);

// Sprites

bool resource_sprite_save(const char* path, const struct Sprite* sprite);

// Returns a malloc'd sprite (one allocation, free() or resource_free works), or NULL
struct Sprite* resource_sprite_load(const char* path);

#endif
