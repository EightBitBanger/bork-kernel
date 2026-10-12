#include <kernel/dwm/windows/taskbar.h>
#include <kernel/dwm/windows/start_menu.h>
#include <kernel/dwm/dwm_core_internal.h>

#include <kernel/util/string.h>
#include <kernel/util/parser.h>

#include <kernel/vfs/vfs.h>
#include <kernel/events.h>

static void taskbar_draw_start_button(void) {
    bool pressed = dwm_start_menu_is_open();
    
    uint32_t fill   = pressed ? theme.button.fill_pressed   : theme.button.fill;
    uint32_t border = pressed ? theme.button.border_pressed : theme.button.border;
    
    // Button body
    dwm_draw_rect_filled(DWM_START_BUTTON_X, DWM_START_BUTTON_Y, DWM_START_BUTTON_W, DWM_START_BUTTON_H, fill);
    
    struct Image* icon = (struct Image*)dwm_resource_find("ui_start");
    
    if (icon != NULL && icon->data != NULL) {
        // Center the sprite inside the button
        int16_t icon_x = DWM_START_BUTTON_X + ((int16_t)DWM_START_BUTTON_W - (int16_t)icon->width)  / 2;
        int16_t icon_y = DWM_START_BUTTON_Y + ((int16_t)taskbar.height    - (int16_t)icon->height) / 2;
        
        // Nudge down the image while pressed for a "pushed in" feel
        if (pressed) icon_y += 2;
        
        dwm_draw_sprite(icon_x, icon_y, icon);
    }
    
    dwm_draw_rect(DWM_START_BUTTON_X, DWM_START_BUTTON_Y, DWM_START_BUTTON_W, DWM_START_BUTTON_H, border);
}

void callback_taskbar_handler(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam) {
    switch (event) {
    case DWM_EVENT_MOUSE: {
        if (!(lparam & DWM_STATE_MOUSE_BTN_LEFT)) break;

        uint16_t click_x = (uint16_t)(wparam & 0xFFFF);
        uint16_t click_y = (uint16_t)((wparam >> 16) & 0xFFFF);

        if (click_x >= DWM_START_BUTTON_X && click_x < DWM_START_BUTTON_X + DWM_START_BUTTON_W &&
            click_y >= DWM_START_BUTTON_Y && click_y < DWM_START_BUTTON_Y + DWM_START_BUTTON_H) {
            dwm_start_menu_toggle();
        }
        break;
    }

    case DWM_EVENT_REDRAW:
        dwm_draw_rect_filled_gradient_vertical(0, -3, display_get_width(), taskbar.height + 7,
                                               theme.taskbar.gradient_low, theme.taskbar.gradient_high);
        taskbar_draw_start_button();
        break;
    }
}
