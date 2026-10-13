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
// Two forms of an image:
//   struct Sprite   palettized, what is stored on disk and compiled in
//   struct Image    32-bit bitmap, what the DWM draws (x86 only)

#include <stdbool.h>

#include <kernel/dwm/rendering/sprite.h>
#include <kernel/resources/resource_class.h>

// Handler registered with the resource manager
extern const ResourceHandler resource_class_image;

// ---- Sprites (the stored form) ----

bool resource_sprite_save(const char* path, const struct Sprite* sprite);

// Returns a malloc'd sprite (one allocation, free() or resource_free works), or NULL
struct Sprite* resource_sprite_load(const char* path);

// ---- Bitmaps (the drawable form) ----

#if defined(KERNEL_PLATFORM_X86)

#include <kernel/dwm/rendering/image.h>

// Expand a sprite (from a file or compiled in) into a new 32-bit bitmap.
// Release with resource_image_free. NULL on bad input or out of memory.
struct Image* resource_image_from_sprite(const struct Sprite* sprite);

// Load a sprite resource file straight into a bitmap. The intermediate
// sprite is freed before this returns. NULL if the file is missing, not a
// sprite resource, or memory runs out.
struct Image* resource_image_load(const char* path);

// Free an Image from resource_image_from_sprite / resource_image_load
// (the struct and its pixel buffer). NULL is ignored.
void resource_image_free(struct Image* image);

#endif

#endif
