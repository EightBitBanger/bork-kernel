#ifndef _WINDOW_OBJECT_H_
#define _WINDOW_OBJECT_H_

#include <stdint.h>
#include <kernel/dwm/configuration.h>
#include <kernel/dwm/objects/window_handle.h>
#include <kernel/dwm/dwm_events.h>

struct WindowObject {
    WindowHandle id;
    
    uint16_t flags;
    uint16_t style;
    char title[DWM_MAX_TITLE_LEN];
    
    // Child windows
    struct WindowObject* parent;
    struct list_node* children_head;
    struct list_node* children_tail;
    
    // Buttons
    struct list_node* buttons_head;
    struct list_node* buttons_tail;
    
    // Editable text fields
    struct list_node* edit_head;
    struct list_node* edit_tail;
    
    // Resources
    struct map_node* resource_head;
    struct map_node* resource_tail;
    
    void(*event_callback)(WindowHandle, wEvent, uint32_t wparam, int32_t lparam);
    
    // Position and size
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
    
    uint16_t local_x;
    uint16_t local_y;
    uint16_t max_width;
    uint16_t max_height;
    
    // Drawing area
    uint16_t surface_x;
    uint16_t surface_y;
    uint16_t surface_w;
    uint16_t surface_h;
    
    // Client frame buffer
    uint32_t* frame_buffer;
    uint16_t buffer_w;
    uint16_t buffer_h;
    
    // Layout
    uint8_t border_width;
    uint8_t titlebar_height;
    
    // Colors
    uint32_t border_color;
    uint32_t background_color;
    uint32_t title_text_color;
    
    uint32_t title_color_low;
    uint32_t title_color_high;
    
    uint32_t inactive_color_low;
    uint32_t inactive_color_high;
};

#endif
