#include <kernel/dwm/windows/dialog_file.h>
#include <kernel/dwm/dwm_core_internal.h>

#include <kernel/util/string.h>
#include <kernel/malloc.h>
#include <kernel/vfs/vfs.h>
#include <kernel/events.h>

struct FileDialogState {
    uint8_t mode;                      // DIALOG_FILE_MODE_SAVE or DIALOG_FILE_MODE_LOAD
    EditFieldHandle edit_handle;      // Handle for filename entry box
    FileDialogCallback callback;       // User completion callback
    char current_dir[DWM_MAX_PATH_LEN];
    char target_file[128];
    bool completed;                    // Callback already ran (Enter twice, click + key, ...)
};

WindowHandle dwm_summon_file_dialog(const char* title, const char* initial_path, uint8_t mode, FileDialogCallback callback) {
    DWM_GUARD();
    WindowClass wclass;
    uint16_t width  = 360;
    uint16_t height = 180;
    
    wclass.width  = width;
    wclass.height = height;
    wclass.x      = (display_get_width() - width) / 2;
    wclass.y      = (display_get_height() - height) / 2;
    wclass.max_width  = width;
    wclass.max_height = height;
    
    strncpy(wclass.title, title, DWM_MAX_TITLE_LEN - 1);
    wclass.title[DWM_MAX_TITLE_LEN - 1] = '\0';
    
    struct WindowObject* dialog = dwm_allocate_window(
        wclass,
        0,
        (WindowProcedure)callback_file_dialog_handler
    );
    
    if (!dialog) return 0;
    
    struct FileDialogState* state = (struct FileDialogState*)malloc(sizeof(struct FileDialogState));
    if (state == NULL) {
        dwm_destroy_window(dialog->id);
        return 0;
    }
    memset(state, 0, sizeof(struct FileDialogState));
    
    state->mode = mode;
    state->callback = callback;
    
    if (initial_path != NULL && initial_path[0] != '\0') {
        strncpy(state->current_dir, initial_path, DWM_MAX_PATH_LEN - 1);
    } else {
        strncpy(state->current_dir, "/", DWM_MAX_PATH_LEN - 1);
    }
    
    // Add input field for target file name
    EditFieldHandle edit = dwm_window_add_edit_field(dialog->id, 100, 70, 230);
    dwm_window_edit_visible(dialog->id, edit, true);
    state->edit_handle = edit;
    
    dwm_window_resource_add(dialog->id, "dialog_state", state);
    dwm_set_focus(dialog);
    
    return dialog->id;
}

static void complete_file_dialog(WindowHandle handle, struct FileDialogState* state, bool cancelled) {
    // Enter can arrive again before the CLOSE below is dispatched
    if (state->completed) return;
    state->completed = true;
    
    if (state->callback) {
        char full_output_path[DWM_MAX_PATH_LEN];
        memset(full_output_path, 0, DWM_MAX_PATH_LEN);
        
        if (!cancelled && state->edit_handle != 0) {
            char filename[128] = {0};
            dwm_window_edit_get_text(handle, state->edit_handle, filename, 127);
            
            strncpy(full_output_path, state->current_dir, DWM_MAX_PATH_LEN - 1);
            size_t len = strlen(full_output_path);
            
            if (len > 0 && full_output_path[len - 1] != '/') {
                strncat(full_output_path, "/", DWM_MAX_PATH_LEN - len - 1);
            }
            strncat(full_output_path, filename, DWM_MAX_PATH_LEN - strlen(full_output_path) - 1);
        }
        
        state->callback(full_output_path, cancelled);
    }
    
    // `state` is a window resource: dwm_destroy_window frees it. Freeing it
    // here too (and again on DESTROY) was a double free -> slab crashout.
    dwm_window_send_event(handle, DWM_EVENT_CLOSE);
}

void callback_file_dialog_handler(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam) {
    struct WindowObject* window = dwm_get_window_by_id(handle);
    struct FileDialogState* state = (struct FileDialogState*)dwm_window_resource_get_by_name(handle, "dialog_state");
    
    if (!window || !state) return;
    
    // UI Dimensions
    uint16_t btn_w = 70;
    uint16_t btn_h = 24;
    
    uint16_t ok_x     = window->w - (btn_w * 2 + 20);
    uint16_t cancel_x = window->w - (btn_w + 10);
    uint16_t btn_y    = window->h - (btn_h + 12);
    
    // Color Palette
    uint32_t color_bg            = theme.client.background;
    uint32_t color_text_primary  = theme.client.text;
    uint32_t color_text_value    = theme.client.text_value;
    uint32_t color_border_normal = theme.button.border;
    uint32_t color_fill_button   = theme.button.fill;
    uint32_t color_divider       = theme.client.accent;
    
    const char* action_label = (state->mode == DIALOG_FILE_MODE_SAVE) ? "Save" : "Open";
    
    switch (event) {
        case DWM_EVENT_KEYBOARD:
            // ESC key -> Cancel
            if ((wparam & 0xFF) == 0x1B) {
                complete_file_dialog(handle, state, true);
            }
            // ENTER key -> Confirm
            else if ((wparam & 0xFF) == 0x02) {
                complete_file_dialog(handle, state, false);
            }
            break;
            
        case DWM_EVENT_REDRAW:
            // Background & Divider
            dwm_draw_rect_filled(0, 0, window->w, window->h, color_bg);
            dwm_draw_line(0, 30, window->w, 0, color_divider);
            
            // Path & Label Text
            dwm_draw_text(15, 10, "Location:", color_text_primary);
            dwm_draw_text(90, 10, state->current_dir, color_text_value);
            
            dwm_draw_text(15, 74, "File Name:", color_text_primary);
            
            // Action Button (Save / Open)
            dwm_draw_rect_filled(ok_x, btn_y, btn_w, btn_h, color_fill_button);
            dwm_draw_rect(ok_x, btn_y, btn_w, btn_h, color_border_normal);
            dwm_draw_text(ok_x + ((btn_w - (strlen(action_label) * 6)) / 2), btn_y + 8, action_label, color_text_value);
            
            // Cancel Button
            dwm_draw_rect_filled(cancel_x, btn_y, btn_w, btn_h, color_fill_button);
            dwm_draw_rect(cancel_x, btn_y, btn_w, btn_h, color_border_normal);
            dwm_draw_text(cancel_x + ((btn_w - 36) / 2), btn_y + 8, "Cancel", color_text_primary);
            break;
            
        case DWM_EVENT_MOUSE: {
            if (lparam & DWM_STATE_MOUSE_BTN_RIGHT) break;
            
            uint16_t click_x = (uint16_t)(wparam & 0xFFFF);
            uint16_t click_y = (uint16_t)((wparam >> 16) & 0xFFFF);
            
            // Clicked Action Button
            if (click_x >= ok_x && click_x < (ok_x + btn_w) &&
                click_y >= btn_y && click_y < (btn_y + btn_h)) {
                complete_file_dialog(handle, state, false);
            }
            // Clicked Cancel Button
            else if (click_x >= cancel_x && click_x < (cancel_x + btn_w) &&
                    click_y >= btn_y && click_y < (btn_y + btn_h)) {
                complete_file_dialog(handle, state, true);
            }
            break;
        }
        
        default:
            break;
    }
}
