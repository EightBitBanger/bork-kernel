#ifndef _KERNEL_RESOURCE_IMAGE_H_
#define _KERNEL_RESOURCE_IMAGE_H_

// Image resources (RESOURCE_TYPE_SPRITE)
//
//   Sprite payload, same order as struct Sprite (little-endian)
//     0   u16      width
//     2   u16      height
//     4   u16      palette_size
//     6   u16[]    data: palette_size * 2 words of palette (each COLOR() is
//                  two words), followed by width * height palette indices
//
//   payload_size = 6 + 2 * (palette_size * 2 + width * height)
//
// The in-memory form is struct Sprite (resources/classes/sprite.h).
// sprite_get_bitmap() expands one to 32-bit 0xAARRGGBB pixels.

#include <stdbool.h>

#include <kernel/resources/classes/sprite.h>
#include <kernel/resources/resource_class.h>

// Handler registered with the resource manager
extern const ResourceHandler resource_class_image;

bool resource_sprite_save(const char* path, const struct Sprite* sprite);

// Returns a malloc'd sprite (one allocation, free() or resource_free works), or NULL
struct Sprite* resource_sprite_load(const char* path);

#endif

