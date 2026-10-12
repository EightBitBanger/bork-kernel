#include <kernel/dwm/rendering/sprite.h>

#include <stddef.h>


void sprite_get_bitmap(uint32_t* bitmap, const struct Sprite* sprite) {
    if (!bitmap || !sprite) return;
    const uint32_t* palette = (const uint32_t*)sprite->data;
    
    size_t pixel_offset = sprite->palette_size * 2;
    const uint16_t* pixel_data = &sprite->data[pixel_offset];
    
    size_t total_pixels = sprite->width * sprite->height;
    for (size_t i = 0; i < total_pixels; i++) {
        uint16_t index = pixel_data[i];
        if (index < sprite->palette_size) {
            bitmap[i] = palette[index];
        } else {
            bitmap[i] = 0x00000000;
        }
    }
}
