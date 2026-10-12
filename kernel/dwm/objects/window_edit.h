#ifndef _WINDOW_EDIT_FIELD_H_
#define _WINDOW_EDIT_FIELD_H_

#include <stdint.h>

struct WindowEditField {
    EditFieldHandle id;
    
    bool is_active;
    bool is_centered;
    
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
    
    uint16_t cursor_index;
    
    char* text;
};

#endif
