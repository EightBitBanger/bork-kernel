#ifndef _KERNEL_RESOURCE_SPRITE_H_
#define _KERNEL_RESOURCE_SPRITE_H_

// Palettized sprite: the stored form of an image resource
// (RESOURCE_TYPE_SPRITE, see classes/image.h for the file payload).
//
// Fields are in the same order as the payload. data[] holds palette_size
// palette entries, two words each (low half first, see COLOR() in the
// drawing headers), followed by width * height palette indices, row by row.
// Palette entries are 0xAARRGGBB; see-through pixels use opaque black
// (SYS_GFX_SPRITE_KEY).

#include <stdint.h>

struct Sprite {
    uint16_t width;
    uint16_t height;
    uint16_t palette_size;
    const uint16_t data[];
};

// Expand `sprite` into width * height 0xAARRGGBB pixels at `out` (stride =
// width). An index past the palette comes out as opaque black (the key).
void sprite_get_bitmap(uint32_t* out, const struct Sprite* sprite);

#endif
