#include <kernel/dwm/windows/message_error.h>
#include <kernel/dwm/dwm_core_internal.h>

#include <kernel/util/string.h>
#include <kernel/util/parser.h>

#include <kernel/vfs/vfs.h>
#include <kernel/events.h>

WindowHandle dwm_summon_message_error(const char* title, const char* message, const char* details, const char* icon) {
    DWM_GUARD();
    WindowClass wclass_msgbox;
    uint16_t width  = 300;
    uint16_t height = 150;
    
    wclass_msgbox.width  = width;
    wclass_msgbox.height = height;
    wclass_msgbox.x      = (display_get_width() - width) / 2;
    wclass_msgbox.y      = (display_get_height() - height) / 2;
    
    wclass_msgbox.max_width  = width;
    wclass_msgbox.max_height = height;
    
    strncpy(wclass_msgbox.title, title, DWM_MAX_TITLE_LEN - 1);
    wclass_msgbox.title[DWM_MAX_TITLE_LEN - 1] = '\0';
    
    struct WindowObject* msg_handle = dwm_allocate_window(
        wclass_msgbox, 
        0, 
        (WindowProcedure)callback_message_error_handler
    );
    
    // Allocate and assign message string
    size_t message_length = strnlen(message, 1024);
    char* rc_message = (char*)malloc(message_length + 1);
    if (rc_message != NULL) {
        strncpy(rc_message, message, message_length);
        rc_message[message_length] = '\0';
    }
    
    size_t details_length = strnlen(message, 1024);
    if (details_length > 0) {
        char* rc_details = (char*)malloc(details_length + 1);
        strncpy(rc_details, details, details_length);
        rc_details[details_length] = '\0';
        
        dwm_window_resource_add(msg_handle->id, "details", rc_details);
    }
    
    // Allocate and map the type string to match existing framework labels
    char* rc_type = (char*)malloc(DWM_MAX_NAME_LEN);
    strncpy(rc_type, icon, DWM_MAX_NAME_LEN);
    
    // Allocate memory to hold the icon index value
    char* rc_icon_name = (char*)malloc(DWM_MAX_PATH_LEN);
    if (rc_icon_name != NULL) {
        strncpy(rc_icon_name, icon, DWM_MAX_PATH_LEN);
        dwm_window_resource_add(msg_handle->id, "image", rc_icon_name);
    }
    
    // Bind all data assets to the window context identifier lists
    dwm_window_resource_add(msg_handle->id, "text", rc_message);
    dwm_window_resource_add(msg_handle->id, "type", rc_type);
    
    
    dwm_set_focus(msg_handle);
    return msg_handle->id;
}

void callback_message_error_handler(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam) {
    
    const uint16_t msg_x           = 108;
    const uint16_t msg_y           = 38;
    
    const uint16_t button_x        = 120;
    const uint16_t button_w        = 60;
    const uint16_t button_h        = 24;
    const uint16_t button_padding  = 14;
    
    const uint16_t label_x         = 90;
    const uint16_t label_y         = 48;
    
    const uint16_t sprite_x        = 40;
    const uint16_t sprite_y        = 22;
    
    const uint32_t color_bg        = theme.client.background;
    const uint32_t color_btn       = theme.button.fill;
    const uint32_t color_border    = theme.button.border;
    const uint32_t color_text      = theme.button.text;
    const uint32_t color_label     = theme.client.text;
    
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) return;

    // Dynamically calculate button_y from the bottom edge of the window
    uint16_t button_y = window->h - button_h - button_h - button_padding;
    
    switch (event) {
        case DWM_EVENT_REDRAW:
            // Background
            dwm_draw_rect_filled(0, 0, window->w, window->h, color_bg);
            
            // Message string text
            char* message_resource = (char*)dwm_window_resource_get_by_name(handle, "text");
            if (message_resource != NULL) {
                dwm_draw_text(msg_x, msg_y, message_resource, color_label);
            }
            
            // Render OK button box
            dwm_draw_rect_filled(button_x, button_y, button_w, button_h, color_btn);
            dwm_draw_rect(button_x, button_y, button_w, button_h, color_border);
            
            // Center text inside button bounds (Character height asset is ~8)
            const char* button_text = "ok"; 
            uint16_t text_width = strlen(button_text) * 6;
            uint16_t text_x = button_x + ((button_w - text_width) / 2);
            uint16_t text_y = button_y + ((button_h - 8) / 2);
            dwm_draw_text(text_x, text_y, button_text, color_text);
            
            // Meta type category text label
            char* type_str = (char*)dwm_window_resource_get_by_name(handle, "details");
            if (type_str != NULL) {
                dwm_draw_text(label_x, label_y, type_str, color_label);
            }
            
            // Extract & map sprite resource
            char* image_name = (char*)dwm_window_resource_get_by_name(handle, "image");
            
            struct Image* sprite = (struct Image*)dwm_resource_find(image_name);
            if (sprite != NULL) {
                dwm_draw_sprite(sprite_x, sprite_y, sprite);
            }
            break;
            
        case DWM_EVENT_MOUSE: {
            uint16_t click_x = (uint16_t)(wparam & 0xFFFF);
            uint16_t click_y = (uint16_t)((wparam >> 16) & 0xFFFF);
            
            if (click_x >= button_x && click_x < (button_x + button_w) && 
                click_y >= button_y && click_y < (button_y + button_h)) {
                dwm_window_send_event(handle, DWM_EVENT_CLOSE);
            }
            break;
        }
            
        case DWM_EVENT_KEYBOARD:
            if (wparam == 0x1B) { // ESC key
                dwm_window_send_event(handle, DWM_EVENT_CLOSE);
            }
            break;
            
        default:
            break;
    }
}
