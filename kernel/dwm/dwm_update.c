#include <kernel/dwm/dwm.h>
#include <kernel/dwm/dwm_core_internal.h>
#include <kernel/console/keyboard.h>
#include <kernel/console/display.h>
#include <kernel/util/list.h>
#include <kernel/util/timer.h>
#include <kernel/util/string.h>

// Route the current mouse snapshot to whichever interaction is active
static void dwm_apply_mouse_state(struct WindowContext* ctx) {
    if (dragdrop.dragged_window != NULL) {
        dwm_update_window_dragging(ctx);
    } else if (dragdrop.dragged_resizing != NULL) {
        dwm_update_window_resizing(ctx);
    } else if (dragdrop.dragged_icon != NULL) {
        dwm_update_icon_dragging(ctx);
    } else if (dragdrop.captured_window != 0) {
        dwm_update_window_capture(ctx);
    } else {
        dwm_update_mouse(ctx);
    }
}

void dwm_update(void) {
    // Turn last frame's focus changes into FOCUS_LOST / FOCUS_GAINED messages
    dwm_process_focus_change();
    
    // Process Window Messages
    DWMMessage msg;
    while (dwm_get_message(&msg)) {
        dwm_dispatch_message(&msg);
    }
    
    // Notification timeouts and slide animations
    dwm_notification_tick();
    dwm_start_menu_tick();
    
    // Invalidate the OLD cursor position
    dwm_invalidate_region(input.mouse_last.x, input.mouse_last.y, context.window_context.cursor_width, context.window_context.cursor_height);
    
    // Process Hardware Input Queue
    MouseEvent m_event;
    
    // While a window/icon is being dragged or a window resized, every queued
    // motion event used to move it and invalidate its old and new rects, so a
    // frame that drained N events repainted the window ~2N times. Only the
    // final position matters on screen, so plain motion is deferred and
    // applied once. A button transition flushes the deferred position first,
    // so press/release still lands exactly where it happened.
    bool drag_pending = false;
    
    // Drain the queue of all events that happened since the last tick
    while (mouse_dequeue_event(&m_event)) {
        
        bool dragging = (dragdrop.dragged_window   != NULL) ||
                        (dragdrop.dragged_resizing != NULL) ||
                        (dragdrop.dragged_icon     != NULL) ||
                        (dragdrop.captured_window  != 0);
        
        bool buttons_changed = (m_event.left_button  != context.window_context.left_button_pressed) ||
                               (m_event.right_button != context.window_context.right_button_pressed);
        
        // Apply the deferred motion at the previous position before the
        // button change is processed
        if (drag_pending && buttons_changed) {
            dwm_apply_mouse_state(&context.window_context);
            drag_pending = false;
        }
        
        // Update context based on this specific snapshot in time
        context.window_context.mouse.x = m_event.x;
        context.window_context.mouse.y = m_event.y;
        context.window_context.left_button_pressed  = m_event.left_button;
        context.window_context.right_button_pressed = m_event.right_button;
        
        if (dragging && !buttons_changed) {
            drag_pending = true;
            continue;
        }
        
        // Update UI logic for THIS specific event
        dwm_apply_mouse_state(&context.window_context);
    }
    
    if (drag_pending) {
        dwm_apply_mouse_state(&context.window_context);
    }
    
    // Invalidate the new cursor position
    dwm_invalidate_region(context.window_context.mouse.x, context.window_context.mouse.y, context.window_context.cursor_width, context.window_context.cursor_height);
    
    // Draw and flush the screen
    dwm_draw_desktop(&context.window_context);
    
    for (int i = 0; i < context.window_context.dirty_count; i++) {
        struct Rect r = context.window_context.dirty_regions[i];
        draw_flush_region(r.x, r.y, r.w, r.h);
    }
    
    // Reset frame state
    input.mouse_last = context.window_context.mouse;
    input.last_left_button_pressed = context.window_context.left_button_pressed;
    input.last_right_button_pressed = context.window_context.right_button_pressed;
    
    context.window_context.cursor_width   = images.current_cursor.width;
    context.window_context.cursor_height  = images.current_cursor.height;
    context.window_context.dirty_count = 0;
}
