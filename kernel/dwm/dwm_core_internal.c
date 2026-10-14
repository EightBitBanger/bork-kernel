#include <kernel/dwm/dwm.h>
#include <kernel/dwm/dwm_core_internal.h>

#include <kernel/console/display.h>
#include <kernel/events.h>

#include <kernel/resources/classes/image.h>

#include <kernel/util/string.h>
#include <kernel/util/list.h>

bool dwm_create_context_menu(int x, int y, uint32_t directive, const char* items[], int item_count) {
    if (item_count <= 0 || item_count > 16) { 
        return false;
    }
    
    // Reset or start at the root menu layer (layer 0)
    ctxmenu.menu_count = 1;
    struct ContextMenu* menu = &ctxmenu.menus[0];
    
    // Set up standard menu dimensions and styling details
    menu->visible = true;
    menu->x = x;
    menu->y = y;
    menu->item_height = 22;
    menu->item_count = item_count;
    menu->w = 130; // Standard base width, could dynamically scale if needed
    menu->h = menu->item_height * menu->item_count;
    menu->hovered_item = -1; // Clear hover state on initial pop
    
    ctxmenu.menu_directive = directive;
    
    // Copy item names over safely
    for (int i = 0; i < item_count; i++) {
        // Safe string copy limiting to the ContextMenu item array name buffer limits
        strncpy(menu->item[i].name, items[i], sizeof(menu->item[i].name) - 1);
        menu->item[i].name[sizeof(menu->item[i].name) - 1] = '\0';
    }
    
    // Clamp the menu coordinates so it doesn't spawn off-screen
    int display_w = display_get_width();
    int display_h = display_get_height();
    
    // Taskbar height
    if (menu->x + menu->w > display_w) {
        menu->x = display_w - menu->w;
    }
    if (menu->y + menu->h > display_h - taskbar.height) {
        menu->y = display_h - taskbar.height - menu->h;
    }
    
    // Redraw the context menu area
    dwm_invalidate_region(menu->x, menu->y, menu->w, menu->h);
    
    return true;
}

struct WindowObject* dwm_get_window_by_id(uint32_t id) {
    if (id == 0) return NULL;
    for (struct list_node* node = workspace.window_head; node != NULL; node = node->next) {
        struct WindowObject* window = (struct WindowObject*)node->data;
        if (window->id == id) return window;
    }
    return NULL;
}

bool dwm_window_edit_text(WindowHandle handle, EditFieldHandle edit_field_handle, const char* text) {
    DWM_GUARD();
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) 
        return 0;
    for (struct list_node* node = window->edit_head; node != NULL; node = node->next) {
        struct WindowEditField* field = (struct WindowEditField*)node->data;
        
        if (field->id != edit_field_handle) 
            continue;
        size_t len = strnlen(text,32);
        strncpy(field->text, text, 128);
        
        field->cursor_index = len;
        
        return true;
    }
    return false;
}

bool dwm_window_edit_visible(WindowHandle handle, EditFieldHandle edit_field_handle, bool enable) {
    DWM_GUARD();
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) 
        return 0;
    for (struct list_node* node = window->edit_head; node != NULL; node = node->next) {
        struct WindowEditField* field = (struct WindowEditField*)node->data;
        
        if (field->id != edit_field_handle) 
            continue;
        
        field->is_active = enable;
        return true;
    }
    return false;
}


bool dwm_window_edit_insert(WindowHandle handle, EditFieldHandle edit_field_handle, const char* text) {
    DWM_GUARD();
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) 
        return false; // Changed to false for consistency with bool return type
    
    for (struct list_node* node = window->edit_head; node != NULL; node = node->next) {
        struct WindowEditField* field = (struct WindowEditField*)node->data;
        
        if (field->id != edit_field_handle) 
            continue;
        
        // Calculate lengths
        size_t new_len = strnlen(text, 32); 
        size_t current_len = strnlen(field->text, 128);
        
        // Safety check: ensure cursor isn't out of bounds
        if (field->cursor_index > current_len) {
            field->cursor_index = current_len;
        }
        
        // Prevent buffer overflow (assuming field->text max capacity is 128)
        if (current_len + new_len >= 128) {
            // Either truncate new_len or return false. Let's cap it to fit.
            if (127 > current_len) {
                new_len = 127 - current_len;
            } else {
                return false; // No room at all
            }
        }
        
        // Shift the trailing text down to make room
        // We use memmove since the source and destination regions overlap
        size_t num_chars_to_shift = current_len - field->cursor_index;
        
        memmove(
            &field->text[field->cursor_index + new_len], // Where to move it to
            &field->text[field->cursor_index],           // Where it is now
            num_chars_to_shift + 1                       // +1 to include the null terminator '\0'
        );
        
        // Copy the new text into the newly created gap
        memcpy(&field->text[field->cursor_index], text, new_len);
        
        // Advance the cursor
        field->cursor_index += new_len;
        return true;
    }
    return false;
}

bool dwm_window_edit_backspace(WindowHandle handle, EditFieldHandle edit_field_handle) {
    DWM_GUARD();
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) 
        return false;
    
    for (struct list_node* node = window->edit_head; node != NULL; node = node->next) {
        struct WindowEditField* field = (struct WindowEditField*)node->data;
        
        if (field->id != edit_field_handle) 
            continue;
        
        size_t current_len = strnlen(field->text, 128);
        
        // Safety check: ensure cursor isn't out of bounds
        if (field->cursor_index > current_len) {
            field->cursor_index = current_len;
        }
        
        // If the cursor is at the very beginning, there is nothing to backspace
        if (field->cursor_index == 0) {
            return false;
        }
        
        // Calculate how many characters need to be shifted left
        // (everything from the cursor position to the end, including the null terminator)
        size_t num_chars_to_shift = current_len - field->cursor_index;
        
        // Shift trailing text one position to the left over the deleted character
        memmove(
            &field->text[field->cursor_index - 1], 
            &field->text[field->cursor_index], 
            num_chars_to_shift + 1 // Include null char
        );
        
        field->cursor_index--;
        return true;
    }
    
    return false;
}

bool dwm_window_edit_cursor_set_pos(WindowHandle handle, EditFieldHandle edit_field_handle, uint16_t position) {
    DWM_GUARD();
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) 
        return 0;
    for (struct list_node* node = window->edit_head; node != NULL; node = node->next) {
        struct WindowEditField* field = (struct WindowEditField*)node->data;
        
        if (field->id != edit_field_handle) 
            continue;
        
        field->cursor_index = position;
        return true;
    }
    return false;
}

bool dwm_window_edit_set_pos(WindowHandle handle, EditFieldHandle edit_field_handle, uint16_t x, uint16_t y) {
    DWM_GUARD();
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) 
        return 0;
    for (struct list_node* node = window->edit_head; node != NULL; node = node->next) {
        struct WindowEditField* field = (struct WindowEditField*)node->data;
        
        if (field->id != edit_field_handle) 
            continue;
        
        field->x = x;
        field->y = y;
        return true;
    }
    return false;
}

bool dwm_window_edit_set_width(WindowHandle handle, EditFieldHandle edit_field_handle, uint16_t width) {
    DWM_GUARD();
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) 
        return 0;
    for (struct list_node* node = window->edit_head; node != NULL; node = node->next) {
        struct WindowEditField* field = (struct WindowEditField*)node->data;
        
        if (field->id != edit_field_handle) 
            continue;
        
        field->width = width;
        return true;
    }
    return false;
}

size_t dwm_window_edit_get_len(WindowHandle handle, EditFieldHandle edit_field_handle) {
    DWM_GUARD();
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) 
        return 0;
    for (struct list_node* node = window->edit_head; node != NULL; node = node->next) {
        struct WindowEditField* field = (struct WindowEditField*)node->data;
        
        if (field->id != edit_field_handle) 
            continue;
        
        return strnlen(field->text, 32);
    }
    return 0;
}

bool dwm_window_edit_get_text(WindowHandle handle, EditFieldHandle edit_field_handle, char* name_buffer, size_t buffer_size) {
    DWM_GUARD();
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) 
        return false;
    for (struct list_node* node = window->edit_head; node != NULL; node = node->next) {
        struct WindowEditField* field = (struct WindowEditField*)node->data;
        
        if (field->id != edit_field_handle) 
            continue;
        
        strncpy(name_buffer, field->text, buffer_size);
        
        return true;
    }
    return false;
}

uint16_t dwm_window_get_width(WindowHandle handle) {
    DWM_GUARD();
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) 
        return 0;
    return window->w;
}

uint16_t dwm_window_get_height(WindowHandle handle) {
    DWM_GUARD();
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) 
        return 0;
    return window->h;
}

uint8_t dwm_window_set_name(WindowHandle handle, const char* name) {
    DWM_GUARD();
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) 
        return 0;
    strncpy(window->title, name, DWM_MAX_TITLE_LEN);
    return 1;
}

void dwm_set_focus(struct WindowObject* target) {
    if (target == NULL) return;
    
    // Any explicit window focus takes it back from the desktop
    workspace.desktop_focused = false;
    
    if (workspace.window_tail != NULL && workspace.window_tail->data == target) {
        return; 
    }
    
    if (workspace.window_tail != NULL) {
        struct WindowObject* old_active = (struct WindowObject*)workspace.window_tail->data;
        old_active->flags |= DWM_WFLAG_REDECORATE;
        
    }
    
    if (list_remove(&workspace.window_head, &workspace.window_tail, target)) {
        list_append(&workspace.window_head, &workspace.window_tail, target);
    }
}

// Clicking empty desktop: no window is focused, the z-order is left as is
void dwm_set_desktop_focus(void) {
    workspace.desktop_focused = true;
}

// The window that currently has input focus, or NULL when the desktop has it
struct WindowObject* dwm_get_focused_window(void) {
    if (workspace.desktop_focused) return NULL;
    if (workspace.window_tail == NULL) return NULL;
    return (struct WindowObject*)workspace.window_tail->data;
}

// Called once per frame from dwm_update (outside the DWM mutex). Compares the
// current focus against last frame's and posts FOCUS_LOST / FOCUS_GAINED.
// Detecting the change here instead of inside dwm_set_focus catches every
// path that moves focus (clicks, window creation, destruction, desktop
// clicks) in one place, once per frame.
void dwm_process_focus_change(void) {
    struct WindowObject* focused = dwm_get_focused_window();
    WindowHandle current = focused ? focused->id : 0;
    
    if (current == workspace.last_focus) return;
    
    WindowHandle previous = workspace.last_focus;
    workspace.last_focus = current;
    
    // Repaint both titlebars so active/inactive colors follow focus
    struct WindowObject* old_window = dwm_get_window_by_id(previous);
    if (old_window != NULL) {
        old_window->flags |= DWM_WFLAG_REDECORATE;
        int ax, ay;
        dwm_get_absolute_position(old_window, &ax, &ay);
        dwm_invalidate_region(ax - old_window->border_width, ay - old_window->border_width,
                              old_window->w + (old_window->border_width * 2),
                              old_window->h + (old_window->border_width * 2));
        dwm_post_message(previous, DWM_EVENT_FOCUS_LOST, current, 0);
    }
    
    if (focused != NULL) {
        focused->flags |= DWM_WFLAG_REDECORATE;
        int ax, ay;
        dwm_get_absolute_position(focused, &ax, &ay);
        dwm_invalidate_region(ax - focused->border_width, ay - focused->border_width,
                              focused->w + (focused->border_width * 2),
                              focused->h + (focused->border_width * 2));
        dwm_post_message(current, DWM_EVENT_FOCUS_GAINED, previous, 0);
    }
}

// Client area of `window` that falls inside the screen rectangle given,
// copied from its frame buffer to the back buffer
void dwm_upload_window_buffer(struct WindowObject* window, int clip_x, int clip_y, int clip_w, int clip_h) {
    if (window == NULL || clip_w <= 0 || clip_h <= 0) return;

    GfxSurface surface;
    dwm_window_surface(window, &surface);

    // Buffer pixel (0, 0) sits at the top-left of the client area
    int client_x = window->x;
    int client_y = window->y + window->titlebar_height;

    GfxRect src = gfx_rect(clip_x - client_x, clip_y - client_y, clip_w, clip_h);
    GfxContext* gc = dwm_screen_context();

    GfxBlend old = gfx_set_blend(gc, GFX_BLEND_COPY);
    gfx_blit(gc, &surface, &src, clip_x, clip_y);
    gfx_set_blend(gc, old);
}

void dwm_window_set_parent(WindowHandle child, WindowHandle parent) {
    DWM_GUARD();
    struct WindowObject* w_parent = dwm_get_window_by_id(parent);
    struct WindowObject* w_child  = dwm_get_window_by_id(child);
    
    if (w_parent == NULL || w_child == NULL) 
        return;
    
    w_child->style |= DWM_WSTYLE_CHILD;
    
    if (w_child->parent != NULL) {
        list_remove(&w_child->parent->children_head, &w_child->parent->children_tail, w_child);
    }
    
    // If local offsets are 0, migrate the window's original creation coordinates
    // to serve as the local relative offsets inside the parent canvas.
    if (w_child->local_x == 0 && w_child->local_y == 0) {
        w_child->local_x = w_child->x;
        w_child->local_y = w_child->y;
    }
    
    w_child->parent = w_parent;
    list_append(&w_parent->children_head, &w_parent->children_tail, w_child);
    
    w_child->x = w_parent->surface_x + w_child->local_x;
    w_child->y = w_parent->surface_y + w_child->local_y;
    
    // Recalculate child's own drawing canvas metrics based on its updated global position
    w_child->surface_x = w_child->x;
    w_child->surface_y = w_child->y + w_child->titlebar_height + (w_child->border_width ? 1 : 0);
    w_child->surface_w = w_child->w;
    w_child->surface_h = w_child->h - w_child->titlebar_height;
    
    // Trigger paint/refresh pipeline using the correct screen coordinates
    w_child->flags |= (DWM_WFLAG_REDRAW | DWM_WFLAG_REFRESH);
    
    dwm_invalidate_region(w_child->x - w_child->border_width, 
                          w_child->y - w_child->border_width, 
                          w_child->w + (w_child->border_width * 2), 
                          w_child->h + (w_child->border_width * 2));
}

WindowHandle dwm_window_get_parent(WindowHandle window) {
    DWM_GUARD();
    struct WindowObject* handle = dwm_get_window_by_id(window);
    if (handle == NULL) 
        return 0;
    if (handle->parent == NULL) 
        return 0;
    return handle->parent->id;
}

void dwm_window_set_focus(WindowHandle handle) {
    DWM_GUARD();
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) return;
    
    // If someone clicks a child window directly, focus its top-level parent instead
    if (window->parent != NULL) {
        window = window->parent;
    }
    
    workspace.desktop_focused = false;
    
    // Move the parent to the end of the top-level z-order list
    if (workspace.window_tail != NULL && (struct WindowObject*)workspace.window_tail->data != window) {
        list_remove(&workspace.window_head, &workspace.window_tail, window);
        list_append(&workspace.window_head, &workspace.window_tail, window);
        
        // Force the whole group to repaint
        window->flags |= DWM_WFLAG_REFRESH;
        dwm_invalidate_region(window->x - window->border_width, window->y - window->border_width,
                              window->w + (window->border_width * 2), window->h + (window->border_width * 2));
    }
}

WindowHandle dwm_window_get_focus(void) {
    DWM_GUARD();
    struct WindowObject* focused_window = dwm_get_focused_window();
    return focused_window ? focused_window->id : 0;
}

// Register a compiled-in sprite as a drawable image (the expansion lives in
// the image resource class, shared with images loaded from disk)
void dwm_resource_sprite_load(const char* resource_name, const struct Sprite* sprite) {
    struct Image* img = resource_image_from_sprite(sprite);
    if (img == NULL) return;

    dwm_resource_load(resource_name, img);
}

void dwm_window_send_event(WindowHandle handle, wEvent event) {
    DWM_GUARD();
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) return;

    uint32_t wparam = 0;
    if (event == DWM_EVENT_RESIZE) {
        wparam = ((uint32_t)(uint16_t)window->h << 16) | ((uint32_t)(uint16_t)window->w & 0xFFFF);
    }

    dwm_post_message(handle, event, wparam, 0);
}

// Picked up by dwm_dispatch_message right after the CLOSE handler returns
void dwm_window_cancel_close(WindowHandle handle) {
    DWM_GUARD();
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window != NULL) window->flags |= DWM_WFLAG_KEEP_OPEN;
}

uint32_t dwm_window_get_count(void) {
    DWM_GUARD();
    uint32_t count = 0;
    for (struct list_node* node = workspace.window_head; node != NULL; node = node->next) 
        count++;
    
    return count;
}

void window_add_button(struct WindowObject* window, int16_t x, int16_t y, uint16_t width, uint16_t height, uint16_t event, struct Image* sprite) {
    if (window == NULL) return;
    
    struct WindowButton* button = malloc(sizeof(struct WindowButton));
    if (button == NULL) return;
    
    button->x = x;
    button->y = y;
    button->width = width;
    button->height = height;
    
    button->event = event;
    if (sprite == NULL) {
        button->sprite.data = NULL;
    } else {
        button->sprite = *sprite;
    }
    
    list_append(&window->buttons_head, &window->buttons_tail, button);
}

void dwm_set_keyboard_char(uint16_t ch) {
    DWM_GUARD();
    input.last_key_pressed = ch;
}

uint16_t dwm_get_titlebar_height(WindowHandle handle) {
    DWM_GUARD();
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) return 0;
    return window->titlebar_height;
}

// The clipboard lives in dwm_clipboard.c

// Tell every open window that folder contents changed. Explorer windows
// re-read their folder on DWM_EVENT_REFRESH; every other window ignores it.
//
// Posting to each window directly matters: KEVENT_DWM_REFRESH on its own
// does not reach explorer windows while the desktop or another window (such
// as the copy window) holds focus, so copies, moves and desktop creations
// never showed up in the target folder's window.
//
// Takes the DWM lock. Worker threads should call it through dwm_invoke().
void dwm_refresh_all_windows(void) {
    DWM_GUARD();
    for (struct list_node* node = workspace.window_head; node != NULL; node = node->next) {
        struct WindowObject* window = (struct WindowObject*)node->data;
        dwm_post_message(window->id, DWM_EVENT_REFRESH, 0, 0);
    }

    // Anything else listening for the kernel event still gets it. Queued:
    // the kernel lock must not be taken while the DWM lock is held.
    dwm_kernel_event_send(KEVENT_DWM_REFRESH, "", "");
}

