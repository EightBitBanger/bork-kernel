#include <kernel/dwm/dwm.h>
#include <kernel/dwm/dwm_core_internal.h>
#include <kernel/console/display.h>
#include <kernel/util/list.h>
#include <kernel/util/string.h>

extern struct ClippingPlane clipping_plain;
extern struct MultibootInfo* vinfo;
extern uint32_t* front_buffer;
extern uint32_t* back_buffer;
extern uint32_t* frame_buffer;

extern const uint8_t char_rom[];

bool rects_intersect(int x1, int y1, int w1, int h1, int x2, int y2, int w2, int h2);
void get_rect_intersection(int x1, int y1, int w1, int h1, int x2, int y2, int w2, int h2, int *out_x, int *out_y, int *out_w, int *out_h);

struct WindowObject* dwm_get_root_parent(struct WindowObject* window) {
    struct WindowObject* root = window;
    while (root->parent != NULL) {
        root = root->parent;
    }
    return root;
}

void dwm_sync_child_positions(struct WindowObject* parent) {
    if (parent == NULL) return;
    
    struct list_node* current = parent->children_head;
    while (current != NULL) {
        struct WindowObject* child = (struct WindowObject*)current->data;
        
        // Update child's absolute positions using its local offsets and parent surface bounds
        child->x = parent->surface_x + child->local_x;
        child->y = parent->surface_y + child->local_y;
        
        // Re-calculate the child's client surface bounds based on its new absolute position
        child->surface_x = child->x;
        child->surface_y = child->y + child->titlebar_height + (child->border_width ? 1 : 0);
        
        // Propagate updates further down the layout tree if this child has children
        dwm_sync_child_positions(child);
        
        current = current->next;
    }
}

void dwm_render_window_recursive(struct WindowObject* window, const struct WindowContext* ctx, uint32_t* frame_buffer, uint32_t screen_stride) {
    if (window == NULL) return;

    if (window->flags & DWM_WFLAG_REDRAW) { 
        window->flags &= ~DWM_WFLAG_REDRAW;
        draw_set_clip_rect(0, 0, window->buffer_w, window->buffer_h);
        draw_set_buffer(window->frame_buffer, window->buffer_w, window->buffer_h);
        
        context.event_window = window;
        if (window->event_callback != NULL) {
            window->event_callback(window->id, DWM_EVENT_REDRAW, 0, 0);
        }
        context.event_window = NULL;

        draw_set_clip_rect(0, 0, display_get_width(), display_get_height());
        draw_set_buffer_default();
    }
    
    int win_min_x = window->x - window->border_width;
    int win_min_y = window->y - window->border_width;
    int win_w = window->w + (window->border_width * 2);
    int win_h = window->h + (window->border_width * 2);

    bool dirty_intersection = false;
    bool needs_redecorate = (window->flags & DWM_WFLAG_REDECORATE) != 0;
    
    for (int i = 0; i < ctx->dirty_count; i++) {
        struct Rect r = ctx->dirty_regions[i];
        if (rects_intersect(r.x, r.y, r.w, r.h, win_min_x, win_min_y, win_w, win_h)) {
            dirty_intersection = true;
            break;
        }
    }
    
    if (!needs_redecorate && dirty_intersection) {
        int client_x = window->x;
        int client_y = window->y + window->titlebar_height;
        int client_w = window->w;
        int client_h = window->h - window->titlebar_height;
        for (int i = 0; i < ctx->dirty_count; i++) {
            struct Rect r = ctx->dirty_regions[i];
            if (rects_intersect(r.x, r.y, r.w, r.h, win_min_x, win_min_y, win_w, win_h)) {
                if (r.x < client_x || r.y < client_y || 
                    (r.x + r.w) > (client_x + client_w) || 
                    (r.y + r.h) > (client_y + client_h)) {
                    needs_redecorate = true;
                    break;
                }
            }
        }
    }
    
    int do_redraw = dirty_intersection || (window->flags & (DWM_WFLAG_REDRAW | DWM_WFLAG_REFRESH | DWM_WFLAG_REDECORATE));
    
    if (do_redraw) {
        if ((window->flags & (DWM_WFLAG_REFRESH | DWM_WFLAG_REDECORATE)) || dirty_intersection) {
            if (window->parent == NULL) {
                bool uploaded_chunk = false;
                for (int i = 0; i < ctx->dirty_count; i++) {
                    struct Rect r = ctx->dirty_regions[i];
                    int clip_x, clip_y, clip_w, clip_h;
                    
                    get_rect_intersection(r.x, r.y, r.w, r.h,
                                          window->x, window->y + window->titlebar_height,
                                          window->w, window->h - window->titlebar_height,
                                          &clip_x, &clip_y, &clip_w, &clip_h);
                    if (clip_w > 0 && clip_h > 0) {
                        dwm_upload_window_buffer_to_backbuffer(window, frame_buffer, screen_stride, clip_x, clip_y, clip_w, clip_h);
                        uploaded_chunk = true;
                    }
                }
                
                if (!uploaded_chunk && (window->flags & DWM_WFLAG_REFRESH)) {
                    dwm_upload_window_buffer_to_backbuffer(window, frame_buffer, screen_stride, 
                                                           window->x, window->y + window->titlebar_height, 
                                                           window->w, window->h - window->titlebar_height);
                }
            } else {
                struct WindowObject* root = dwm_get_root_parent(window);
                int child_abs_x, child_abs_y;
                dwm_get_absolute_position(window, &child_abs_x, &child_abs_y);
                
                int clip_x = child_abs_x;
                int clip_y = child_abs_y;
                int clip_w = window->buffer_w;
                int clip_h = window->buffer_h;
                
                get_rect_intersection(clip_x, clip_y, clip_w, clip_h,
                                      window->parent->surface_x, window->parent->surface_y,
                                      window->parent->surface_w, window->parent->surface_h,
                                      &clip_x, &clip_y, &clip_w, &clip_h);
                if (clip_w > 0 && clip_h > 0) {
                    dwm_upload_window_buffer_to_backbuffer(root, frame_buffer, screen_stride, clip_x, clip_y, clip_w, clip_h);
                }
            }
            
            if (needs_redecorate) {
                dwm_draw_window(window);
            }
            
            window->flags &= ~(DWM_WFLAG_REFRESH | DWM_WFLAG_REDECORATE);
        }
    }
    
    struct list_node* current = window->children_head;
    while (current != NULL) {
        struct WindowObject* child = (struct WindowObject*)current->data;
        dwm_render_window_recursive(child, ctx, frame_buffer, screen_stride);
        current = current->next;
    }
}


void dwm_draw_desktop(const struct WindowContext* ctx) {
    // Window contents are copied into the BACK buffer, which is packed with
    // a stride of the display width (see draw_set_buffer_default). The
    // hardware pitch only applies to the front buffer; using it here skews
    // every row (and overruns the back buffer) when the pitch is padded.
    const uint32_t screen_stride = display_get_width();
    
    // Clear out background pixels for all localized dirty regions
    for (int i = 0; i < ctx->dirty_count; i++) {
        struct Rect r = ctx->dirty_regions[i];
        draw_rect_filled(r.x, r.y, r.w, r.h, theme.desktop.background);
    }
    
    uint16_t bar_x = 0;
    uint16_t bar_y = display_get_height() - taskbar.height;
    uint16_t bar_w = display_get_width();
    uint16_t bar_h = taskbar.height;
    
    for (int i = 0; i < ctx->dirty_count; i++) {
        struct Rect r = ctx->dirty_regions[i];
        if (rects_intersect(r.x, r.y, r.w, r.h, bar_x, bar_y, bar_w, bar_h)) {
            draw_rect_filled(bar_x, bar_y, bar_w, bar_h, theme.desktop.background);
            break;
        }
    }
    
    // Draw desktop icons
    
    struct list_node* current_node = workspace.icon_head;
    while (current_node != NULL) {
        struct IconObject* current_icon = (struct IconObject*)current_node->data;
        
        int icon_full_x = current_icon->x + current_icon->bounds_x;
        int icon_full_y = current_icon->y + current_icon->bounds_y;
        
        bool in_refresh_zone = false;
        for (int i = 0; i < ctx->dirty_count; i++) {
            struct Rect r = ctx->dirty_regions[i];
            if (rects_intersect(r.x, r.y, r.w, r.h, icon_full_x, icon_full_y, current_icon->bounds_w, current_icon->bounds_h)) {
                in_refresh_zone = true;
                break;
            }
        }
        
        if (in_refresh_zone) {
            if (current_icon->icon_sprite != NULL) {
                draw_sprite_blend(current_icon->icon_sprite->data, current_icon->width, current_icon->height, current_icon->x, current_icon->y, 0xFF000000);
            } else {
                draw_rect_filled(current_icon->x, current_icon->y, current_icon->width, current_icon->height, theme.desktop.icon_text);
            }
            
            // The rename box replaces the label while the icon is renamed
            if (current_icon != dwm_desktop_rename_icon()) {
                uint16_t text_y = current_icon->y + 45; 
                size_t length = strlen(current_icon->name);
                uint16_t string_width = length * 6;
                int16_t text_start_x = current_icon->x + ((current_icon->width - string_width) / 2);
                
                draw_text(text_start_x, text_y, current_icon->name, theme.desktop.icon_text);
            }
        }
        
        current_node = current_node->next;
    }
    
    // Desktop rename box (above the icons, below every window)
    dwm_desktop_rename_draw(ctx);
    
    // Sync positions before drawing
    for (struct list_node* node = workspace.window_head; node != NULL; node = node->next) {
        struct WindowObject* window = (struct WindowObject*)node->data;
        if (window->parent == NULL) {
            dwm_sync_child_positions(window);
        }
    }
    
    // Draw windows
    
    for (struct list_node* node = workspace.window_head; node != NULL; node = node->next) {
        struct WindowObject* window = (struct WindowObject*)node->data;
        
        // Only process root windows that are NOT topmost and NOT children
        if ((window->style & DWM_WSTYLE_TOPMOST) || (window->parent != NULL) || (window->style & DWM_WSTYLE_CHILD)) 
            continue;
        
        dwm_render_window_recursive(window, ctx, back_buffer, screen_stride);
    }
    
    // Draw top most windows
    
    struct WindowObject* taskbar_window = dwm_get_window_by_id(taskbar.window);
    
    for (struct list_node* node = workspace.window_head; node != NULL; node = node->next) {
        struct WindowObject* window = (struct WindowObject*)node->data;
        
        // Only process root windows that ARE topmost and NOT children
        if (!(window->style & DWM_WSTYLE_TOPMOST) || (window->parent != NULL) || (window->style & DWM_WSTYLE_CHILD)) {
            continue;
        }
        
        // The taskbar goes on last (below)
        if (window == taskbar_window) continue;
        
        dwm_render_window_recursive(window, ctx, back_buffer, screen_stride);
    }
    
    // Taskbar over every other topmost window, whatever the z-order list says
    // (clicking it moves it to the tail). The start menu slides out from
    // behind it this way instead of across it.
    if (taskbar_window != NULL && taskbar_window->parent == NULL) {
        dwm_render_window_recursive(taskbar_window, ctx, back_buffer, screen_stride);
    }
    
    // Context menus
    
    for (uint8_t m = 0; m < ctxmenu.menu_count; m++) {
        struct ContextMenu* menu = &ctxmenu.menus[m];
        if (!menu->visible) continue;
        
        uint16_t menu_height = menu->item_height * menu->item_count;
        menu->h = menu_height;
        
        draw_rect_filled(menu->x, menu->y, menu->w, menu_height, menu->color_bg);
        draw_rect(menu->x, menu->y, menu->w, menu_height, menu->color_border);
        
        for (unsigned int i = 0; i < menu->item_count; i++) {
            uint16_t intersector_height = menu->item_height * i;
            uint16_t menu_x = menu->x + 1;
            uint16_t menu_y = menu->y + intersector_height;
            uint16_t menu_w = menu->w - 3;
            uint16_t menu_h = menu->item_height;
            uint16_t text_x = menu_x + 3;
            uint16_t text_y = menu_y + 7;
            
            if ((int)i == menu->hovered_item) {
                draw_rect_filled(menu_x, menu_y + 1, menu_w + 1, menu_h - 2, menu->color_highlight);
            }
            
            if (i > 0 && (int)i != menu->hovered_item && (int)(i - 1) != menu->hovered_item) {
                draw_line(menu_x, menu_y, menu_x + menu_w, menu_y, menu->color_separator);
            }
            
            draw_text(text_x, text_y, menu->item[i].name, menu->color_text);
        }
    }
    
    // Redraw cursor sprite
    draw_sprite_blend(images.current_cursor.data, ctx->cursor_width, ctx->cursor_height, ctx->mouse.x, ctx->mouse.y, 0xFF000000);
}

void dwm_draw_window(struct WindowObject* window_handle) {
    if (window_handle->style & DWM_WSTYLE_NOBORDERS) 
        return;
    
    int32_t wx = window_handle->x;
    int32_t wy = window_handle->y;
    int32_t ww = window_handle->w;
    int32_t wh = window_handle->h;
    
    struct WindowObject* focused_window = dwm_get_focused_window();
    
    uint32_t current_title_color_low = (window_handle == focused_window) ? window_handle->title_color_low : window_handle->inactive_color_low;
    uint32_t current_title_color_high = (window_handle == focused_window) ? window_handle->title_color_high : window_handle->inactive_color_high;
    
    if (window_handle->titlebar_height > 0) {
        draw_rect_filled(wx, wy, ww, window_handle->titlebar_height, current_title_color_low);
        draw_text(wx + 7, wy + 6, window_handle->title, window_handle->title_text_color);
    }
    
    // Draw titlebar divider
    int div_x = window_handle->x;
    int div_y = window_handle->y + window_handle->titlebar_height - 1;
    int div_w = window_handle->x + window_handle->w;
    int div_h = window_handle->y + window_handle->titlebar_height - 1;
    
    if (window_handle->titlebar_height > 0)
        draw_line(div_x, div_y, div_w, div_h, window_handle->border_color);
    
    // Draw the window outer borders
    for (uint8_t b = 1; b <= window_handle->border_width; b++) {
        
        draw_rect(window_handle->x - b, window_handle->y - b, window_handle->w + (b * 2), window_handle->h + (b * 2), window_handle->border_color);
    }
    
    // Draw window button
    for (struct list_node* node = window_handle->buttons_head; node != NULL; node = node->next) {
        struct WindowButton* btn = (struct WindowButton*)node->data;
        
        if (btn->sprite.data == NULL) 
            continue;
        
        int32_t btn_abs_x = wx + btn->x;
        int32_t btn_abs_y = wy + btn->y;
        
        draw_sprite_blend(btn->sprite.data, btn->sprite.width, btn->sprite.height, btn_abs_x, btn_abs_y, 0xFF000000);
    }
    
    // Draw window editable text fields
    int32_t surface_abs_x = window_handle->surface_x;
    int32_t surface_abs_y = window_handle->surface_y;
    
    for (struct list_node* node = window_handle->edit_head; node != NULL; node = node->next) {
        struct WindowEditField* field = (struct WindowEditField*)node->data;
        if (!field->is_active) 
            continue;
        
        int32_t field_abs_x = surface_abs_x + field->x;
        int32_t field_abs_y = surface_abs_y + field->y;
        
        uint32_t box_bg        = theme.edit.background;
        uint32_t box_bdr       = theme.edit.border;
        uint32_t text_color    = theme.edit.text;
        uint32_t cursor_color  = theme.edit.cursor;
        
        uint16_t font_width    = 6;
        uint16_t font_height   = 8; // Defined height for vertical alignment
        
        // Calculate horizontal text centering offset
        int32_t text_len = strlen(field->text);
        int32_t text_offset_x=0;
        
        if (field->is_centered) {
            int32_t total_text_width = text_len * font_width;
            text_offset_x = (field->width - total_text_width) / 2;
        } else {
            text_offset_x = 5;
        }
        
        if (text_offset_x < 0) text_offset_x = 0; // Prevent clipping boundaries if string is too long
        
        // Calculate vertical text centering offset
        int32_t text_offset_y = (field->height - font_height) / 2;
        if (text_offset_y < 0) text_offset_y = 0;
        
        // Apply offsets to get the final text drawing positions
        int32_t text_draw_x = field_abs_x + text_offset_x;
        int32_t text_draw_y = field_abs_y + text_offset_y;
        
        // Draw the background boxes
        draw_rect_filled(field_abs_x, field_abs_y, field->width, field->height, box_bg);
        draw_rect(field_abs_x-1, field_abs_y-1, field->width+2, field->height+2, box_bdr);
        
        // Draw the centered text
        draw_text(text_draw_x, text_draw_y, field->text, text_color);
        
        // Update the cursor position relative to the centered text coordinates
        uint16_t cursor_index = (field->cursor_index * font_width);
        
        uint16_t cursor_x = text_draw_x + cursor_index;
        uint16_t cursor_y = text_draw_y;
        uint16_t cursor_w = text_draw_x + cursor_index;
        uint16_t cursor_h = text_draw_y + font_height;
        
        // Draw cursor line
        draw_line(cursor_x, cursor_y, cursor_w, cursor_h, cursor_color);
        draw_line(cursor_x+1, cursor_y, cursor_w+1, cursor_h, cursor_color);
    }
    
}

//
// Low level drawing
//

void dwm_draw_line(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color) {
    draw_line(x, y, x + w, y + h, color);
}

void dwm_draw_text(int16_t x, int16_t y, const char* text, uint32_t color) {
    if (context.event_window == NULL) return;
    size_t length = strlen(text);
    for (unsigned int i=0; i < length; i++) 
        draw_glyph(char_rom, text[i], 6, 8, x + (i * 6), y, color, 0xFF000000, 0xFF000000);
}

void dwm_draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color) {
    if (context.event_window == NULL) return;
    draw_rect(x, y, w, h, color);
}

void dwm_draw_rect_filled(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color) {
    if (context.event_window == NULL) return;
    draw_rect_filled(x, y, w, h, color);
}

void dwm_draw_rect_filled_gradient_vertical(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color_low, uint32_t color_high) {
    if (context.event_window == NULL) return;
    draw_rect_gradient_vertical_blend(x, y, w, h, color_high, color_low);
}

void dwm_draw_rect_filled_gradient_horizontal(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color_low, uint32_t color_high) {
    if (context.event_window == NULL) return;
    draw_rect_gradient_horizontal_blend(x, y, w, h, color_high, color_low);
}

void dwm_draw_sprite(int16_t x, int16_t y, struct Image* image) {
    if (context.event_window == NULL) return;
    draw_sprite_blend(image->data, image->width, image->height, x, y, 0x00000000);
}

void dwm_set_cursor(uint32_t* sprite, int16_t width, int16_t height) {
    images.current_cursor.data = sprite;
    images.current_cursor.width = width;
    images.current_cursor.height = height;
}
