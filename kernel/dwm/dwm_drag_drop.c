#include <kernel/dwm/dwm.h>
#include <kernel/dwm/dwm_core_internal.h>
#include <kernel/console/display.h>
#include <kernel/util/list.h>
#include <kernel/util/string.h>
#include <kernel/util/timer.h>

// Window buffer resize multiple
#define DWM_BUFFER_CHUNK_SIZE 256

void dwm_update_icon_dragging(struct WindowContext* ctx) {
    if (dragdrop.dragged_icon == NULL) return;
    if (dragdrop.dragged_icon == NULL) return;

    if (ctx->left_button_pressed) {
        // Calculate distance from initial click
        int dx = ctx->mouse.x - dragdrop.drag_start_x;
        int dy = ctx->mouse.y - dragdrop.drag_start_y;

        // Require at least 4 pixels of movement before starting the drag (4^2 = 16)
        if (!dragdrop.is_dragging) {
            if ((dx * dx + dy * dy) < 16) {
                return; // Suppress micro-jitter during clicks
            }
            dragdrop.is_dragging = true;
        }
        
        int new_x = ctx->mouse.x - dragdrop.icon_drag_offset_x;
        int new_y = ctx->mouse.y - dragdrop.icon_drag_offset_y;
        int display_w = display_get_width();
        int display_h = display_get_height();
        
        if (new_x + dragdrop.dragged_icon->bounds_x < 0) new_x = -dragdrop.dragged_icon->bounds_x;
        if (new_x + dragdrop.dragged_icon->bounds_x + dragdrop.dragged_icon->bounds_w > display_w) 
            new_x = display_w - dragdrop.dragged_icon->bounds_x - dragdrop.dragged_icon->bounds_w;
        
        if (new_y + dragdrop.dragged_icon->bounds_y < 0) new_y = -dragdrop.dragged_icon->bounds_y;
        if (new_y + dragdrop.dragged_icon->bounds_y + dragdrop.dragged_icon->bounds_h > display_h) 
            new_y = display_h - dragdrop.dragged_icon->bounds_y - dragdrop.dragged_icon->bounds_h;
        
        if (dragdrop.dragged_icon->x != new_x || dragdrop.dragged_icon->y != new_y) {
            dwm_invalidate_region(dragdrop.dragged_icon->x + dragdrop.dragged_icon->bounds_x, 
                                  dragdrop.dragged_icon->y + dragdrop.dragged_icon->bounds_y, 
                                  dragdrop.dragged_icon->bounds_w, dragdrop.dragged_icon->bounds_h);
            
            dragdrop.dragged_icon->x = new_x;
            dragdrop.dragged_icon->y = new_y;
            
            dwm_invalidate_region(dragdrop.dragged_icon->x + dragdrop.dragged_icon->bounds_x, 
                                  dragdrop.dragged_icon->y + dragdrop.dragged_icon->bounds_y, 
                                  dragdrop.dragged_icon->bounds_w, 
                                  dragdrop.dragged_icon->bounds_h);
        }
    } else {
        
        dragdrop.dragged_icon = NULL;
    }
}

void dwm_update_window_dragging(struct WindowContext* ctx) {
    if (dragdrop.dragged_window == NULL) return;
    
    if (ctx->left_button_pressed) {
        int new_x = ctx->mouse.x - dragdrop.drag_offset_x;
        int new_y = ctx->mouse.y - dragdrop.drag_offset_y;
        
        int display_w = display_get_width();
        int display_h = display_get_height();
        
        // Constrain window dragging boundaries
        if (new_x - (int)dragdrop.dragged_window->border_width < 0) new_x = dragdrop.dragged_window->border_width;
        if (new_x + (int)dragdrop.dragged_window->w + (int)dragdrop.dragged_window->border_width > display_w) new_x = display_w - dragdrop.dragged_window->w - dragdrop.dragged_window->border_width;
        if (new_y - (int)dragdrop.dragged_window->border_width < 0) new_y = dragdrop.dragged_window->border_width;
        if (new_y + (int)dragdrop.dragged_window->h + (int)dragdrop.dragged_window->border_width > display_h) new_y = display_h - dragdrop.dragged_window->h - dragdrop.dragged_window->border_width;
        
        if (dragdrop.dragged_window->x != new_x || dragdrop.dragged_window->y != new_y) {
            dwm_invalidate_region(dragdrop.dragged_window->x - dragdrop.dragged_window->border_width, dragdrop.dragged_window->y - dragdrop.dragged_window->border_width, dragdrop.dragged_window->w + (dragdrop.dragged_window->border_width * 2), dragdrop.dragged_window->h + (dragdrop.dragged_window->border_width * 2));
            
            dragdrop.dragged_window->x = new_x;
            dragdrop.dragged_window->y = new_y;
            
            if (dragdrop.dragged_window->parent != NULL) {
                dragdrop.dragged_window->local_x = new_x - dragdrop.dragged_window->parent->surface_x;
                dragdrop.dragged_window->local_y = new_y - dragdrop.dragged_window->parent->surface_y;
            } else {
                dragdrop.dragged_window->local_x = 0;
                dragdrop.dragged_window->local_y = 0;
            }
            
            dragdrop.dragged_window->surface_x = new_x;
            dragdrop.dragged_window->surface_y = new_y + dragdrop.dragged_window->titlebar_height + (dragdrop.dragged_window->titlebar_height ? 1 : 0);
            dragdrop.dragged_window->surface_w = dragdrop.dragged_window->w;
            dragdrop.dragged_window->surface_h = dragdrop.dragged_window->h - dragdrop.dragged_window->titlebar_height;
            
            dwm_cascade_child_positions(dragdrop.dragged_window);
            
            dwm_invalidate_region(dragdrop.dragged_window->x - dragdrop.dragged_window->border_width, dragdrop.dragged_window->y - dragdrop.dragged_window->border_width, dragdrop.dragged_window->w + (dragdrop.dragged_window->border_width * 2), dragdrop.dragged_window->h + (dragdrop.dragged_window->border_width * 2));
            
            dragdrop.dragged_window->flags |= DWM_WFLAG_REFRESH;
        }
    } else {
        // Released - drop the window
        dragdrop.dragged_window = NULL;
    }
}

void dwm_update_window_resizing(struct WindowContext* ctx) {
    if (dragdrop.dragged_resizing == NULL) return;
    if (dragdrop.dragged_resizing->frame_buffer == NULL) 
        return;
    
    if (ctx->left_button_pressed) {
        // Project desired sizing bounds derived from mouse tracking adjustments
        int target_w = (ctx->mouse.x + dragdrop.resize_offset_x) - dragdrop.dragged_resizing->x;
        int target_h = (ctx->mouse.y + dragdrop.resize_offset_y) - dragdrop.dragged_resizing->y;
        
        int display_w = display_get_width();
        int display_h = display_get_height();
        int border_ext = dragdrop.dragged_resizing->border_width;

        // Prevent resizing beyond the right and bottom screen boundaries
        if (dragdrop.dragged_resizing->x + target_w + border_ext > display_w) {
            target_w = display_w - dragdrop.dragged_resizing->x - border_ext;
        }
        if (dragdrop.dragged_resizing->y + target_h + border_ext > display_h) {
            target_h = display_h - dragdrop.dragged_resizing->y - border_ext;
        }
        
        // Enforce a sensible minimum geometry threshold
        int min_width = 64;
        int min_height = 48 + dragdrop.dragged_resizing->titlebar_height;
        
        if (target_w < min_width)  target_w = min_width;
        if (target_h < min_height) target_h = min_height;
        
        // Enforce the maximum geometry threshold if defined (greater than 0)
        if (dragdrop.dragged_resizing->max_width > 0 && target_w > dragdrop.dragged_resizing->max_width) {
            target_w = dragdrop.dragged_resizing->max_width;
        }
        if (dragdrop.dragged_resizing->max_height > 0 && target_h > dragdrop.dragged_resizing->max_height) {
            target_h = dragdrop.dragged_resizing->max_height;
        }
        
        // Check if dimension shifts have actually happened
        if (dragdrop.dragged_resizing->w != target_w || dragdrop.dragged_resizing->h != target_h) {
            
            // Invalidate old window space configuration
            dwm_invalidate_region(dragdrop.dragged_resizing->x - border_ext, 
                                dragdrop.dragged_resizing->y - border_ext, 
                                dragdrop.dragged_resizing->w + (border_ext * 2), 
                                dragdrop.dragged_resizing->h + (border_ext * 2));
            
            // Resize the actual frame buffer
            dwm_resize_window_buffer(dragdrop.dragged_resizing, target_w, target_h);
            
            // Synchronize any nested hierarchical layouts attached to it
            dwm_sync_child_positions(dragdrop.dragged_resizing);
            
            // Event throttle timer
            static uint64_t last_resize_ms = 0;
            uint64_t current_ms = timer_get_ms();
            
            if (current_ms - last_resize_ms >= 100) {
                dwm_window_send_event(dragdrop.dragged_resizing->id, DWM_EVENT_RESIZE);
                last_resize_ms = current_ms;
            }
            
            // Update surface boundaries for the compositor clipping
            int border_offset = 0;
            dragdrop.dragged_resizing->surface_w = dragdrop.dragged_resizing->w;
            dragdrop.dragged_resizing->surface_h = dragdrop.dragged_resizing->h - (dragdrop.dragged_resizing->titlebar_height + border_offset);
            dragdrop.dragged_resizing->surface_x = dragdrop.dragged_resizing->x;
            dragdrop.dragged_resizing->surface_y = dragdrop.dragged_resizing->y + dragdrop.dragged_resizing->titlebar_height + (dragdrop.dragged_resizing->titlebar_height ? 1 : 0);
            
            // Update the positioning coordinates of the resize handle and system buttons
            for (struct list_node* node = dragdrop.dragged_resizing->buttons_head; node != NULL; node = node->next) {
                struct WindowButton* btn = (struct WindowButton*)node->data;
                if (btn->event == DWM_EVENT_RESIZE) {
                    btn->x = dragdrop.dragged_resizing->w - btn->width;
                    btn->y = dragdrop.dragged_resizing->h - btn->height;
                }
                
                if (btn->event == DWM_EVENT_CLOSE) {
                    btn->x = dragdrop.dragged_resizing->w - btn->width;
                }
                if (btn->event == DWM_EVENT_MINIMIZE) {
                    struct Image* button_close = dwm_resource_find("ui_close");
                    if (button_close != NULL) {
                        btn->x = dragdrop.dragged_resizing->w - button_close->width - btn->width;
                    }
                }
            }
            
            // Request a complete paint pass and invalidate the updated boundaries
            dragdrop.dragged_resizing->flags |= (DWM_WFLAG_REFRESH | DWM_WFLAG_REDRAW | DWM_WFLAG_REDECORATE);
            dwm_invalidate_region(dragdrop.dragged_resizing->x - border_ext, 
                                  dragdrop.dragged_resizing->y - border_ext, 
                                  dragdrop.dragged_resizing->w + (border_ext * 2), 
                                  dragdrop.dragged_resizing->h + (border_ext * 2));
        }
    } else {
        // Drag click released - conclude the sizing operations safely
        dragdrop.dragged_resizing = NULL;
    }
}

void dwm_resize_window_buffer(struct WindowObject* window, int new_w, int new_h) {
    if (window == NULL) return;
    
    // Check if we need to expand the actual memory buffer
    if (new_w > window->buffer_w || new_h > window->buffer_h) {
        
        // Calculate new capacity by snapping to our chunk size
        int new_capacity_w = window->buffer_w;
        int new_capacity_h = window->buffer_h;
        
        while (new_capacity_w < new_w) new_capacity_w += DWM_BUFFER_CHUNK_SIZE;
        while (new_capacity_h < new_h) new_capacity_h += DWM_BUFFER_CHUNK_SIZE;
        
        uint32_t* new_buffer = (uint32_t*)malloc(new_capacity_w * new_capacity_h * sizeof(uint32_t));
        
        // Copy the old pixel data into the new buffer
        if (window->frame_buffer != NULL) {
            for (int y = 0; y < window->h; y++) {
                // We MUST copy row-by-row because the buffer_w (stride) has changed!
                memcpy(&new_buffer[y * new_capacity_w], 
                    &window->frame_buffer[y * window->buffer_w], 
                    window->w * sizeof(uint32_t));
            }
            free(window->frame_buffer);
        }
        
        window->frame_buffer = new_buffer;
        window->buffer_w = new_capacity_w;
        window->buffer_h = new_capacity_h;
    }
    
    // Fill the newly exposed "L-shape" with a background color
    // (Wait for the client app to catch up and draw its actual UI)
    uint32_t bg_color = theme.client.background;
    
    // Fill the new bottom rectangle
    if (new_h > window->h) {
        for (int y = window->h; y < new_h; y++) {
            for (int x = 0; x < new_w; x++) {
                window->frame_buffer[y * window->buffer_w + x] = bg_color;
            }
        }
    }
    
    // Fill the new right rectangle
    if (new_w > window->w) {
        for (int y = 0; y < window->h; y++) { // Only go up to the old height (bottom rect handles the rest)
            for (int x = window->w; x < new_w; x++) {
                window->frame_buffer[y * window->buffer_w + x] = bg_color;
            }
        }
    }
    
    // Update logical dimensions
    window->w = new_w;
    window->h = new_h;
    
    // Flag the window so the compositor knows the boundaries changed and needs a redecoration
    window->flags |= (DWM_WFLAG_REDECORATE | DWM_WFLAG_REFRESH);
}

void dwm_cascade_child_positions(struct WindowObject* parent) {
    if (parent == NULL) return;
    
    struct list_node* current = parent->children_head;
    while (current != NULL) {
        struct WindowObject* child = (struct WindowObject*)current->data;
        
        // Invalidate child's old screen space before shifting it
        dwm_invalidate_region(child->x - child->border_width, child->y - child->border_width,
                              child->w + (child->border_width * 2), child->h + (child->border_width * 2));
        
        // Push coordinate shifts relative to parent's newly calculated surface bounds
        child->x = parent->surface_x + child->local_x;
        child->y = parent->surface_y + child->local_y;
        
        child->surface_x = child->x;
        child->surface_y = child->y + child->titlebar_height + (child->border_width ? 1 : 0);
        
        // Mark child for refresh and invalidate its new screen space
        child->flags |= DWM_WFLAG_REFRESH;
        dwm_invalidate_region(child->x - child->border_width, child->y - child->border_width,
                              child->w + (child->border_width * 2), child->h + (child->border_width * 2));
        
        // Recursively cycle through lower sub-menus/hierarchies if they exist
        dwm_cascade_child_positions(child);
        
        current = current->next;
    }
}


// Route pointer motion and the button release to the window that was clicked.
// Motion is coalesced by dwm_update, so at most one MOUSE_MOVE is posted per
// frame (plus one before every button change).
void dwm_update_window_capture(struct WindowContext* ctx) {
    if (dragdrop.captured_window == 0) return;
    
    struct WindowObject* window = dwm_get_window_by_id(dragdrop.captured_window);
    if (window == NULL) {
        dragdrop.captured_window = 0;
        return;
    }
    
    int mx = ctx->mouse.x - window->surface_x;
    int my = ctx->mouse.y - window->surface_y;
    uint32_t position = ((uint32_t)(uint16_t)(int16_t)my << 16) | (uint32_t)(uint16_t)(int16_t)mx;
    
    int32_t state = 0;
    if (ctx->left_button_pressed)  state |= DWM_STATE_MOUSE_BTN_LEFT;
    if (ctx->right_button_pressed) state |= DWM_STATE_MOUSE_BTN_RIGHT;
    
    if (ctx->left_button_pressed) {
        if (ctx->mouse.x != dragdrop.capture_last.x || ctx->mouse.y != dragdrop.capture_last.y) {
            dragdrop.capture_last = ctx->mouse;
            dwm_post_message(window->id, DWM_EVENT_MOUSE_MOVE, position, state);
        }
    } else {
        dwm_post_message(window->id, DWM_EVENT_MOUSE_UP, position, state);
        dragdrop.captured_window = 0;
    }
}
