#ifndef IMAGE_H
#define IMAGE_H

#include <stdint.h>

struct Image {
    uint16_t width;
    uint16_t height;
    
    uint32_t* data;
};

#endif
