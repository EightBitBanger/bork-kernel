#ifndef DWM_WINDOW_CONTEXT_H
#define DWM_WINDOW_CONTEXT_H

#include <kernel/console/mouse.h>

#define MAX_DIRTY_RECTS  32

struct Rect {
    int x, y, w, h;
};

struct WindowContext {
    Point mouse;
    
    struct Rect dirty_regions[MAX_DIRTY_RECTS];
    int dirty_count;
    
    bool left_button_pressed;
    bool right_button_pressed;
    bool is_double_click;
    
    uint32_t cursor_width;
    uint32_t cursor_height;
};

#endif
