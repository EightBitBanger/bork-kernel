#ifndef CONTEXT_MENU_H
#define CONTEXT_MENU_H

struct MenuItem {
    
    char name[16];
};

struct ContextMenu {
    bool visible;
    
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    
    // Colors
    
    uint32_t color_bg;
    uint32_t color_border;
    uint32_t color_separator;
    uint32_t color_highlight;
    uint32_t color_text;
    
    // Items
    
    int hovered_item; // Mouse hover
    
    uint8_t item_height;
    
    uint8_t item_count;
    struct MenuItem item[16];
};

#endif
