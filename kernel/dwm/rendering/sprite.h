#ifndef SPRITE_H
#define SPRITE_H

#include <stdint.h>

struct Sprite {
    uint16_t width;
    uint16_t height;
    
    uint16_t palette_size;
    
    const uint16_t data[];
};

void sprite_get_bitmap(uint32_t* bitmap, const struct Sprite* sprite);

#endif
