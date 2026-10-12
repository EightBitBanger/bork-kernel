#include <kernel/dwm/images/cursor.h>
#include <kernel/dwm/rendering/sprite.h>

#ifdef ADD_IMAGE_LIB

const struct Sprite rc_cursor_pointer = {
    .width  = 11,
    .height = 11,
    .palette_size = 8,
    
    .data = {
        COLOR(0xFF000000),
        COLOR(0xFF909090),
        COLOR(0xFF888888),
        COLOR(0xFF747474),
        COLOR(0xFF606060),
        COLOR(0xFF585858),
        COLOR(0xFF444444),
        COLOR(0xFFFFFFFF),
        
        7,7,7,7,7,7,7,7,7,0,0,
        7,5,4,3,2,1,1,1,1,0,0,
        7,6,4,3,2,2,1,7,0,0,0,
        7,6,4,3,3,2,7,0,0,0,0,
        7,6,4,3,3,2,7,0,0,0,0,
        7,6,4,4,3,3,2,7,0,0,0,
        7,5,5,7,7,3,2,1,7,0,0,
        7,5,7,0,0,7,3,2,1,7,0,
        7,5,0,0,0,0,7,3,2,1,7,
        0,0,0,0,0,0,0,7,4,1,7,
        0,0,0,0,0,0,0,0,7,7,7
    }
};


const struct Sprite rc_cursor_edge = {
    .width  = 11,
    .height = 11,
    .palette_size = 8,
    
    .data = {
        COLOR(0xFF000000),
        COLOR(0xFF909090),
        COLOR(0xFF888888),
        COLOR(0xFF747474),
        COLOR(0xFF606060),
        COLOR(0xFF585858),
        COLOR(0xFF444444),
        COLOR(0xFFFFFFFF),
        
        7,7,7,7,7,7,7,7,7,0,0,
        7,5,4,3,2,1,1,1,1,0,0,
        7,6,4,3,2,2,1,7,0,0,0,
        7,6,4,3,3,2,7,0,0,0,0,
        7,6,4,3,3,7,0,0,0,0,0,
        7,6,4,4,7,0,0,0,0,0,0,
        7,5,5,7,0,0,0,0,0,0,0,
        7,5,7,0,0,0,0,0,0,0,0,
        7,5,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0
    }
};

const struct Sprite rc_cursor_angle = {
    .width  = 11,
    .height = 11,
    .palette_size = 8,
    
    .data = {
        COLOR(0xFF000000),
        COLOR(0xFF909090),
        COLOR(0xFF888888),
        COLOR(0xFF747474),
        COLOR(0xFF606060),
        COLOR(0xFF585858),
        COLOR(0xFF444444),
        COLOR(0xFFFFFFFF),
        
        7,7,7,7,7,7,7,7,7,0,0,
        7,5,4,3,2,1,1,1,1,0,0,
        7,5,0,0,0,0,0,0,0,0,0,
        7,4,0,0,0,0,0,0,0,0,0,
        7,2,0,0,0,0,0,0,0,0,0,
        7,1,0,0,0,0,0,0,0,0,0,
        7,1,0,0,0,0,0,0,0,0,0,
        7,1,0,0,0,0,0,0,0,0,0,
        7,1,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0
    }
};

#endif
