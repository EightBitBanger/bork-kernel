#ifndef _FONT_H_
#define _FONT_H_

#include <stdint.h>

struct Font {
    
    // Character dimensions
    uint16_t width_px;
    uint16_t height_px;
    
    const uint16_t data[];
};

#endif
