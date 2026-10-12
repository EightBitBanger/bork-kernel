#include <kernel/dwm/dwm.h>
#include <kernel/dwm/dwm_core_internal.h>
#include <kernel/events.h>
#include <kernel/console/display.h>
#include <kernel/util/list.h>
#include <kernel/util/timer.h>
#include <kernel/util/string.h>

bool dwm_handle_window_clicks(struct WindowContext* ctx, bool is_new_left_click, bool is_new_right_click);
void dwm_handle_icon_clicks(struct WindowContext* ctx, bool is_new_left_click, bool is_new_right_click);
bool dwm_handle_context_menu_clicks(struct WindowContext* ctx, bool is_new_left_click, bool is_new_right_click);
void dwm_handle_context_menu_hover(struct WindowContext* ctx);

struct WindowObject* dwm_find_clicked_window(struct list_node* tail_node, int mouse_x, int mouse_y);
static struct WindowObject* dwm_find_clicked_window_z(int mouse_x, int mouse_y);

static struct WindowObject* last_clicked_window = NULL;
static uint64_t last_window_click_time = 0;

void dwm_update_mouse(struct WindowContext* ctx) {
    dwm_handle_context_menu_hover(ctx);
    
    bool is_new_left_click = ctx->left_button_pressed && !input.last_left_button_pressed;
    bool is_new_right_click = ctx->right_button_pressed && !input.last_right_button_pressed;
    
    if (!is_new_left_click && !is_new_right_click) return;
    
    if (dwm_handle_context_menu_clicks(ctx, is_new_left_click, is_new_right_click)) 
        return;
    
    // Desktop rename: a click inside the box is swallowed, a click anywhere
    // else commits the new name and is then handled normally
    if (dwm_desktop_rename_handle_click(ctx)) 
        return;
    
    if (dwm_handle_window_clicks(ctx, is_new_left_click, is_new_right_click)) 
        return;
    
    // Clicked outside every window: the desktop takes focus, so the focused
    // window gets DWM_EVENT_FOCUS_LOST (closes popups like the start menu)
    dwm_set_desktop_focus();
    
    dwm_handle_icon_clicks(ctx, is_new_left_click, is_new_right_click);
}

bool dwm_handle_window_clicks(struct WindowContext* ctx, bool is_new_left_click, bool is_new_right_click) {
    struct WindowObject* clicked_win = dwm_find_clicked_window_z(ctx->mouse.x, ctx->mouse.y);
    if (clicked_win == NULL) return false;
    
    // Window double click detection
    if (is_new_left_click) {
        uint64_t current_time = timer_get_ms();
        if (clicked_win == last_clicked_window && 
            (current_time - last_window_click_time) <= ICON_DOUBLE_CLICK_THRESHOLD_MS) {
            context.window_context.is_double_click = true;
            last_clicked_window = NULL;
            last_window_click_time = 0;
        } else {
            context.window_context.is_double_click = false;
            last_clicked_window = clicked_win;
            last_window_click_time = current_time;
        }
    } else {
        context.window_context.is_double_click = false;
    }
    
    // Post mouse event message to target window
    int mx = ctx->mouse.x - clicked_win->surface_x;
    int my = ctx->mouse.y - clicked_win->surface_y;
    uint32_t mouse_data = ((uint32_t)(uint16_t)my << 16) | ((uint32_t)(uint16_t)mx & 0xFFFF);
    int32_t mouse_state = 0;
    if (ctx->left_button_pressed)  mouse_state |= DWM_STATE_MOUSE_BTN_LEFT;
    if (ctx->right_button_pressed) mouse_state |= DWM_STATE_MOUSE_BTN_RIGHT;
    if (context.window_context.is_double_click) mouse_state |= DWM_STATE_MOUSE_DOUBLE_CLK;
    
    dwm_post_message(clicked_win->id, DWM_EVENT_MOUSE, mouse_data, mouse_state);
    
    // Shift focus/z-order tracking to top-level root parent
    struct WindowObject* root_win = clicked_win;
    while (root_win->parent != NULL) {
        root_win = root_win->parent;
    }
    
    struct WindowObject* old_focused = dwm_get_focused_window();
    if (old_focused != root_win) {
        dwm_set_focus(root_win);
        root_win->flags |= (DWM_WFLAG_REFRESH | DWM_WFLAG_REDECORATE);
        if (old_focused) {
            old_focused->flags |= (DWM_WFLAG_REFRESH | DWM_WFLAG_REDECORATE);
            int old_abs_x, old_abs_y;
            dwm_get_absolute_position(old_focused, &old_abs_x, &old_abs_y);
            dwm_invalidate_region(old_abs_x - old_focused->border_width, 
                                  old_abs_y - old_focused->border_width, 
                                  old_focused->w + (old_focused->border_width * 2), 
                                  old_focused->h + (old_focused->border_width * 2));
        }
        
        int root_abs_x, root_abs_y;
        dwm_get_absolute_position(root_win, &root_abs_x, &root_abs_y);
        dwm_invalidate_region(root_abs_x - root_win->border_width, 
                              root_abs_y - root_win->border_width, 
                              root_win->w + (root_win->border_width * 2), 
                              root_win->h + (root_win->border_width * 2));
    }
    
    int clicked_abs_x, clicked_abs_y;
    dwm_get_absolute_position(clicked_win, &clicked_abs_x, &clicked_abs_y);
    
    // Check window buttons
    for (struct list_node* node = clicked_win->buttons_head; node != NULL; node = node->next) {
        struct WindowButton* btn = (struct WindowButton*)node->data;
        int btn_min_x = clicked_win->x + btn->x;
        int btn_max_x = btn_min_x + btn->width;
        int btn_min_y = clicked_win->y + btn->y;
        int btn_max_y = btn_min_y + btn->height;
        if (ctx->mouse.x >= btn_min_x && ctx->mouse.x <= btn_max_x &&
            ctx->mouse.y >= btn_min_y && ctx->mouse.y <= btn_max_y) {
            
            if (btn->event == DWM_EVENT_RESIZE) {
                dragdrop.dragged_resizing = clicked_win;
                dragdrop.resize_offset_x = (clicked_win->x + clicked_win->w) - ctx->mouse.x;
                dragdrop.resize_offset_y = (clicked_win->y + clicked_win->h) - ctx->mouse.y;
                return true;
            } else if (is_new_left_click) {
                dwm_post_message(clicked_win->id, btn->event, 0, 0);
            }
            return true;
        }
    }
    
    // Titlebar drag tracking
    int title_min_x = clicked_abs_x;
    int title_max_x = clicked_abs_x + clicked_win->w;
    int title_min_y = clicked_abs_y;
    int title_max_y = clicked_abs_y + clicked_win->titlebar_height - 1;
    if ((!(clicked_win->style & DWM_WSTYLE_NOBORDERS)) && 
        ctx->mouse.x >= title_min_x && ctx->mouse.x <= title_max_x && 
        ctx->mouse.y >= title_min_y && ctx->mouse.y <= title_max_y) {
        
        dragdrop.drag_offset_x = ctx->mouse.x - clicked_win->x;
        dragdrop.drag_offset_y = ctx->mouse.y - clicked_win->y;
        
        if (is_new_left_click) 
            dragdrop.dragged_window = clicked_win;
    } else if (is_new_left_click) {
        // Client area: keep feeding this window the pointer until release
        // (drag-to-select, sliders, ...)
        dragdrop.captured_window = clicked_win->id;
        dragdrop.capture_last = ctx->mouse;
    }
    
    return true;
}

#include <kernel/vfs/vfs.h>

void dwm_handle_icon_clicks(struct WindowContext* ctx, bool is_new_left_click, bool is_new_right_click) {
    struct IconObject* clicked_icon = NULL;
    // Get the clicked icon
    
    for (struct list_node* node = workspace.icon_head; node != NULL; node = node->next) {
        struct IconObject* icon = (struct IconObject*)node->data;
        int icon_min_x = icon->x + icon->bounds_x;
        int icon_max_x = icon->x + icon->bounds_x + icon->bounds_w;
        int icon_min_y = icon->y + icon->bounds_y;
        int icon_max_y = icon->y + icon->bounds_y + icon->bounds_h;
        
        if (ctx->mouse.x >= icon_min_x && ctx->mouse.x <= icon_max_x &&
            ctx->mouse.y >= icon_min_y && ctx->mouse.y <= icon_max_y) {
            clicked_icon = icon;
            break; 
        }
    }
    
    if (clicked_icon != NULL) {
        context.focused_icon = clicked_icon;
        if (is_new_left_click) {
            uint32_t current_time = timer_get_ms();
            // Calculate spatial distance between clicks
            int dx = ctx->mouse.x - dragdrop.drag_start_x;
            int dy = ctx->mouse.y - dragdrop.drag_start_y;
            bool is_close_enough = (dx * dx + dy * dy) <= 25;
            // Within 5 pixels
            
            if (clicked_icon == context.last_focused_icon && 
                is_close_enough && 
                (current_time - context.last_icon_click_time) <= ICON_DOUBLE_CLICK_THRESHOLD_MS) {
                
                // Register double click
                if (vfs_directory_check(context.focused_icon->path)) {
                    dwm_kernel_event_send(KEVENT_EXECUTE, "explorer", context.focused_icon->path);
                } else if (dwm_path_is_executable(context.focused_icon->path)) {
                    dwm_launch_program(context.focused_icon->path);
                } else {
                    dwm_kernel_event_send(KEVENT_EXECUTE, "notepad", context.focused_icon->path);
                }

                // Reset dwm state
                context.focused_icon = NULL;
                context.last_focused_icon = NULL;
                context.last_icon_click_time = 0;
                dragdrop.dragged_icon = NULL;
            } else {
                // Initialize potential drag state
                dragdrop.dragged_icon = clicked_icon;
                dragdrop.drag_start_x = ctx->mouse.x;
                dragdrop.drag_start_y = ctx->mouse.y;
                dragdrop.is_dragging = false;
                dragdrop.icon_drag_offset_x = ctx->mouse.x - clicked_icon->x;
                dragdrop.icon_drag_offset_y = ctx->mouse.y - clicked_icon->y;
                
                context.last_focused_icon = clicked_icon;
                context.last_icon_click_time = current_time;
            }
        }
        else if (is_new_right_click) { // Icon right click context menu
            
            dwm_icon_right_click(ctx);
        }
    } else { // Desktop click
        
        if (is_new_left_click) {
            context.last_focused_icon = NULL;
        } else if (is_new_right_click) {
            
            // Desktop right click context menu
            
            dwm_desktop_right_click(ctx);
        }
    }
}

bool dwm_handle_context_menu_clicks(struct WindowContext* ctx, bool is_new_left_click, bool is_new_right_click) {
    if (ctxmenu.menu_count == 0) return false;
    
    // Check backwards from the top-most submenu down to root menu
    for (int m = (int)ctxmenu.menu_count - 1; m >= 0; m--) {
        struct ContextMenu* menu = &ctxmenu.menus[m];
        
        if (ctx->mouse.x >= menu->x && ctx->mouse.x <= (menu->x + menu->w) &&
            ctx->mouse.y >= menu->y && ctx->mouse.y <= (menu->y + menu->h)) {
            
            if (is_new_left_click) {
                int relative_y = ctx->mouse.y - menu->y;
                int item_index = relative_y / menu->item_height;
                
                if (item_index >= 0 && item_index < menu->item_count) {
                    dwm_process_context_menu_events(ctx, item_index);
                    
                    // TODO When implementing submenus later, you can intercept here:
                    // If this item opens a submenu, call a push_submenu routine and return true!
                    
                }
            }
            
            // If an option was processed (and it didn't generate a new submenu layer), collapse everything
            for (int i = 0; i < ctxmenu.menu_count; i++) {
                ctxmenu.menus[i].visible = false;
                dwm_invalidate_region(ctxmenu.menus[i].x, ctxmenu.menus[i].y, ctxmenu.menus[i].w, ctxmenu.menus[i].h);
            }
            ctxmenu.menu_count = 0;
            return true;
        }
    }
    
    // Clicked outside all currently open context menus - dismiss everything
    for (int i = 0; i < ctxmenu.menu_count; i++) {
        ctxmenu.menus[i].visible = false;
        dwm_invalidate_region(ctxmenu.menus[i].x, ctxmenu.menus[i].y, ctxmenu.menus[i].w, ctxmenu.menus[i].h);
    }
    ctxmenu.menu_count = 0;
    return false;
}

void dwm_handle_context_menu_hover(struct WindowContext* ctx) {
    if (ctxmenu.menu_count == 0) return;
    
    for (int m = 0; m < ctxmenu.menu_count; m++) {
        struct ContextMenu* menu = &ctxmenu.menus[m];
        if (!menu->visible) continue;
        
        if (ctx->mouse.x >= menu->x && ctx->mouse.x <= (menu->x + menu->w) &&
            ctx->mouse.y >= menu->y && ctx->mouse.y <= (menu->y + menu->h)) {
            
            int relative_y = ctx->mouse.y - menu->y;
            int current_hover = relative_y / menu->item_height;
            
            if (current_hover >= 0 && current_hover < menu->item_count) {
                if (menu->hovered_item != current_hover) {
                    menu->hovered_item = current_hover;
                    dwm_invalidate_region(menu->x, menu->y, menu->w, menu->h);
                }
                // Mouse is accounted for in this menu slot; clear sub-menus layered above it if necessary later
                continue;
            }
        }
        
        // If the mouse left this specific menu boundary segment, clear its highlight
        if (menu->hovered_item != -1) {
            menu->hovered_item = -1;
            dwm_invalidate_region(menu->x, menu->y, menu->w, menu->h);
        }
    }
}

struct WindowObject* dwm_find_clicked_window(struct list_node* tail_node, int mouse_x, int mouse_y) {
    // Traverse backwards through the current z-order level (top-most first)
    for (struct list_node* node = tail_node; node != NULL; node = node->prev) {
        struct WindowObject* window = (struct WindowObject*)node->data;
        
        int abs_x, abs_y;
        dwm_get_absolute_position(window, &abs_x, &abs_y);
        
        int full_min_x = abs_x - window->border_width;
        int full_max_x = abs_x + window->w + window->border_width;
        int full_min_y = abs_y - window->border_width;
        int full_max_y = abs_y + window->h + window->border_width;
        
        if (mouse_x >= full_min_x && mouse_x <= full_max_x &&
            mouse_y >= full_min_y && mouse_y <= full_max_y) {
            
            // Check child window clicks
            if (window->children_tail != NULL) {
                struct WindowObject* clicked_child = dwm_find_clicked_window(window->children_tail, mouse_x, mouse_y);
                if (clicked_child != NULL) {
                    return clicked_child; // A child intercepted the click!
                }
            }
            
            // No children where selected, this window itself is the target
            return window;
        }
    }
    return NULL;
}

// Topmost windows (taskbar, start menu, notifications) are drawn after every
// other window, so they get the first look at a click too. Plain list order
// let a focused normal window steal clicks from a topmost one drawn over it.
static bool dwm_window_hit(struct WindowObject* window, int mouse_x, int mouse_y) {
    int abs_x, abs_y;
    dwm_get_absolute_position(window, &abs_x, &abs_y);
    return mouse_x >= abs_x - window->border_width && mouse_x <= abs_x + window->w + window->border_width &&
           mouse_y >= abs_y - window->border_width && mouse_y <= abs_y + window->h + window->border_width;
}

static struct WindowObject* dwm_find_clicked_window_z(int mouse_x, int mouse_y) {
    // The taskbar is drawn over every other window (dwm_draw_desktop)
    struct WindowObject* bar = dwm_get_window_by_id(taskbar.window);
    if (bar != NULL && bar->parent == NULL && dwm_window_hit(bar, mouse_x, mouse_y))
        return bar;

    for (struct list_node* node = workspace.window_tail; node != NULL; node = node->prev) {
        struct WindowObject* window = (struct WindowObject*)node->data;
        if (!(window->style & DWM_WSTYLE_TOPMOST) || window->parent != NULL) continue;
        if (!dwm_window_hit(window, mouse_x, mouse_y)) continue;

        if (window->children_tail != NULL) {
            struct WindowObject* child = dwm_find_clicked_window(window->children_tail, mouse_x, mouse_y);
            if (child != NULL) return child;
        }
        return window;
    }
    return dwm_find_clicked_window(workspace.window_tail, mouse_x, mouse_y);
}
