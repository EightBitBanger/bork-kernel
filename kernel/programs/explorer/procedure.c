#include <stdio.h>
#include <stdbool.h>

#include <kernel/kernel.h>
#include <kernel/dwm/dwm.h>
#include <kernel/events.h>
#include <kernel/util/string.h>
#include <kernel/malloc.h>
#include <kernel/vfs/vfs.h>

#include <kernel/programs/explorer/internal.h>

static struct ExplorerWindowState* get_window_state(WindowHandle handle);

static void finalize_rename(WindowHandle handle, struct ExplorerWindowState* state) {
    if (state->edit_handle == 0 || state->context_item_index == -1) 
        return;
    
    char new_name[128];
    memset(new_name, '\0', sizeof(new_name));
    
    dwm_window_edit_get_text(handle, state->edit_handle, new_name, 127);
    
    struct Item* target_item = &state->items[state->context_item_index];
    
    // target_item->name was cleared when the rename started, so compare
    // against the last path segment instead
    const char* old_name = strrchr(target_item->path, '/');
    old_name = (old_name != NULL) ? old_name + 1 : target_item->path;
    
    if (strlen(new_name) > 0 && strcmp(old_name, new_name) != 0) {
        if (!vfs_rename(target_item->path, new_name)) {
            dwm_notify("Rename", "Name is invalid, too long or taken", DWM_NOTIFY_LONG_MS);
        }
    }
    
    populate_state_from_vfs(state, state->full_path);
    
    dwm_window_edit_visible(handle, state->edit_handle, false);
    state->context_item_index = -1; 
}

void generate_unique_name(struct ExplorerWindowState* state, char* out_path, const char* default_name) {
    memset(out_path, '\0', 128);
    
    strncpy(out_path, state->full_path, 127);
    size_t base_len = strnlen(out_path, 127);
    
    if (base_len > 0 && out_path[base_len - 1] != '/') {
        out_path[base_len++] = '/';
        out_path[base_len] = '\0';
    }
    
    const char* clean_def = (default_name[0] == '/') ? default_name + 1 : default_name;
    size_t def_len = strnlen(clean_def, 32);
    
    if (base_len + def_len < 127) {
        strncpy(&out_path[base_len], clean_def, def_len);
    }
    
    if (vfs_exists(out_path)) {
        int counter = 1;
        while (1) {
            memset(&out_path[base_len], '\0', 128 - base_len);
            size_t current_len = base_len;
            
            strncpy(&out_path[current_len], clean_def, def_len);
            current_len += def_len;
            
            const char* p1 = " (";
            strncpy(&out_path[current_len], p1, 2);
            current_len += 2;
            
            char num_buf[16];
            memset(num_buf, '\0', 16);
            itos(counter, num_buf);
            
            size_t num_len = strnlen(num_buf, 16);
            strncpy(&out_path[current_len], num_buf, num_len);
            current_len += num_len;
            
            strncpy(&out_path[current_len], ")", 1);
            current_len += 1;
            
            if (!vfs_exists(out_path)) {
                break;
            }
            
            counter++;
            if (current_len >= 120) break;
        }
    }
}

static void handle_explorer_mouse(WindowHandle handle, struct ExplorerWindowState* state, uint32_t wparam, int32_t lparam) {
    uint16_t click_x = (uint16_t)(wparam & 0xFFFF);
    uint16_t click_y = (uint16_t)((wparam >> 16) & 0xFFFF);
    
    // Handle clicking away from rename input field
    if (state->edit_handle != 0 && state->context_item_index != -1) {
        uint16_t max_cols = (state->win_width - NAV_X) / ITEM_WIDTH;
        if (max_cols == 0) max_cols = 1;
        
        uint16_t col = state->context_item_index % max_cols;
        uint16_t row = state->context_item_index / max_cols;
        
        uint16_t item_x = NAV_X + (col * ITEM_WIDTH);
        uint16_t item_y = NAV_Y + (row * ITEM_HEIGHT);
        
        if (click_x < item_x || click_x >= (item_x + ITEM_WIDTH) ||
            click_y < item_y || click_y >= (item_y + ITEM_HEIGHT)) {
            
            finalize_rename(handle, state);
            dwm_window_send_event(handle, DWM_EVENT_REDRAW);
            
            if (!(lparam & DWM_STATE_MOUSE_BTN_RIGHT)) {
                return; 
            }
        }
    }
    
    // Back Button Hit Test
    if (ui_button_back != NULL) {
        uint16_t btn_x1 = BACK_BTN_SPRITE_X;
        uint16_t btn_y1 = BACK_BTN_SPRITE_Y;
        uint16_t btn_x2 = btn_x1 + ui_button_back->width;
        uint16_t btn_y2 = btn_y1 + ui_button_back->height;
        
        if (click_x >= btn_x1 && click_x < btn_x2 && click_y >= btn_y1 && click_y < btn_y2) {
            if (lparam & DWM_STATE_MOUSE_BTN_RIGHT) return;
            
            if (strcmp(state->full_path, "/") != 0) {
                char parent_path[MAX_PATH_LEN];
                strncpy(parent_path, state->full_path, MAX_PATH_LEN - 1);
                parent_path[MAX_PATH_LEN - 1] = '\0';
                
                char* last_slash = strrchr(parent_path, '/');
                if (last_slash != NULL) {
                    if (last_slash == parent_path) {
                        parent_path[1] = '\0'; // Root "/"
                    } else {
                        *last_slash = '\0';
                    }
                } else {
                    strncpy(parent_path, "/", MAX_PATH_LEN - 1);
                }
                
                populate_state_from_vfs(state, parent_path);
                dwm_window_send_event(handle, DWM_EVENT_REDRAW);
            }
            return;
        }
    }
    
    // Grid Items Hit Detection
    uint16_t max_cols = (state->win_width - NAV_X) / ITEM_WIDTH;
    if (max_cols == 0) max_cols = 1;
    
    bool item_hit = false;
    
    for (unsigned int i = 0; i < state->total_items; i++) {
        uint16_t col = i % max_cols;
        uint16_t row = i / max_cols;
        
        uint16_t tile_start_x = NAV_X + (col * ITEM_WIDTH);
        uint16_t tile_start_y = NAV_Y + (row * ITEM_HEIGHT);
        
        if (click_x >= tile_start_x && click_x < (tile_start_x + ITEM_WIDTH) &&
            click_y >= tile_start_y && click_y < (tile_start_y + ITEM_HEIGHT)) {
            
            item_hit = true;
            
            if (lparam & DWM_STATE_MOUSE_BTN_RIGHT) {
                state->context_item_index = (int32_t)i;
                
                const char* item_menu_options[] = { "Open", "Cut", "Copy", "Rename", "Delete", "Properties" };
                dwm_summon_context_menu(handle, click_x, click_y, item_menu_options, 6);
                state->context_directive = 1;
                return;
            }
            
            if (lparam & DWM_STATE_MOUSE_DOUBLE_CLK) {
                struct Item* item = &state->items[i];
                if (vfs_directory_check(item->path)) {
                    populate_state_from_vfs(state, item->path);
                } else if (dwm_path_is_executable(item->path)) {
                    dwm_launch_program(item->path);
                } else {
                    dwm_kernel_event_send(KEVENT_EXECUTE, "notepad", item->path);
                }
                
                dwm_window_send_event(handle, DWM_EVENT_REDRAW);
            }
            break; 
        }
    }
    
    if (!item_hit && (lparam & DWM_STATE_MOUSE_BTN_RIGHT)) {
        const char* window_menu_options[] = { "Refresh", "New Folder", "New File", "Paste", "Properties" };
        state->context_item_index = -1;
        dwm_summon_context_menu(handle, click_x, click_y, window_menu_options, 5);
        state->context_directive = 0;
        return;
    }
}

static void handle_explorer_resize(struct ExplorerWindowState* state, uint32_t wparam) {
    state->win_width = (uint16_t)(wparam & 0xFFFF);
    state->win_height = (uint16_t)((wparam >> 16) & 0xFFFF);
}

static void handle_explorer_redraw(WindowHandle handle, struct ExplorerWindowState* state) {
    const struct DWMTheme* t = dwm_get_theme();
    
    uint16_t window_width = dwm_window_get_width(handle);
    uint16_t window_height = dwm_window_get_height(handle);
    
    dwm_draw_rect_filled(EXPLORER_BG_X, EXPLORER_BG_Y, window_width, window_height, t->client.background);
    
    if (ui_button_back != NULL) {
        dwm_draw_rect_filled(BACK_BTN_CONTAINER_X, BACK_BTN_CONTAINER_Y, BACK_BTN_CONTAINER_W, BACK_BTN_CONTAINER_H, t->edit.background);
        dwm_draw_rect(BACK_BTN_BORDER_X, BACK_BTN_BORDER_Y, BACK_BTN_BORDER_W, BACK_BTN_BORDER_H, t->edit.border);
        dwm_draw_sprite(BACK_BTN_SPRITE_X, BACK_BTN_SPRITE_Y, ui_button_back);
    }
    
    uint16_t path_field_width = window_width - PATH_FIELD_BG_X - 10;
    dwm_draw_rect_filled(PATH_FIELD_BG_X, PATH_FIELD_BG_Y, path_field_width, PATH_FIELD_BG_H, t->edit.background);
    dwm_draw_rect(PATH_FIELD_BORDER_X, PATH_FIELD_BORDER_Y, path_field_width + 2, PATH_FIELD_BORDER_H, t->edit.border);
    
    if (state->fs_current == 0 || state->knode_path_len >= strlen(state->path)) {
        dwm_draw_text(PATH_TEXT_X, PATH_TEXT_Y, state->path, t->edit.text);
    } else {
        char base_buffer[MAX_PATH_LEN];
        memset(base_buffer, 0, MAX_PATH_LEN);
        strncpy(base_buffer, state->path, state->knode_path_len);
        
        dwm_draw_text(PATH_TEXT_X, PATH_TEXT_Y, base_buffer, t->edit.text);
        int16_t mount_offset_x = PATH_TEXT_X + (state->knode_path_len * PATH_FONT_CHAR_WIDTH);
        dwm_draw_text(mount_offset_x, PATH_TEXT_Y, state->path + state->knode_path_len, t->client.text_mount);
    }
    
    dwm_draw_line(NAV_DIVIDER_X, NAV_DIVIDER_Y, window_width, NAV_DIVIDER_H, t->client.divider_soft);
    
    uint16_t max_cols = (state->win_width - NAV_X) / ITEM_WIDTH;
    if (max_cols == 0) max_cols = 1;
    
    for (unsigned int i = 0; i < state->total_items; i++) {
        uint16_t col = i % max_cols;
        uint16_t row = i / max_cols;
        
        uint16_t sp_x = NAV_X + (col * ITEM_WIDTH);
        uint16_t sp_y = (NAV_Y + (row * ITEM_HEIGHT)) + NAV_Y_OFF;
        
        struct Image* current_icon;
        switch (state->items[i].icon_index) {
            case ICON_FOLDER:    current_icon = icon_folder; break;
            case ICON_FILE:      current_icon = icon_file; break;
            case ICON_DOCUMENT:  current_icon = icon_document; break;
            case ICON_SYSTEM:    current_icon = icon_system; break;
            case ICON_STORAGE:   current_icon = icon_storage; break;
            default: current_icon = 0; break;
        }
        
        if (current_icon != NULL) {
            uint16_t icon_offset_x = (ITEM_WIDTH - current_icon->width) / 2; 
            dwm_draw_sprite(sp_x + icon_offset_x, sp_y, current_icon);
        }
        
        size_t length = strlen(state->items[i].name);
        uint16_t string_width = length * ITEM_FONT_CHAR_WIDTH;
        int16_t text_start_x = sp_x + ((ITEM_WIDTH - string_width) / 2);
        dwm_draw_text(text_start_x, sp_y + ITEM_TEXT_HEIGHT_OFF, state->items[i].name, t->client.text_soft);
    }
}

struct ExplorerWindowState* get_window_state(WindowHandle handle) {
    struct ExplorerWindowState* current = window_list_head;
    while (current != NULL) {
        if (current->handle == handle) return current;
        current = current->next;
    }
    return NULL;
}

void callback_handler_explorer(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam) {
    struct ExplorerWindowState* state = get_window_state(handle);
    if (!state) return;
    
    switch (event) {
    case DWM_EVENT_MOUSE:
        handle_explorer_mouse(handle, state, wparam, lparam);
        break;
    
    case DWM_EVENT_KEYBOARD:
        if (state->edit_handle != 0) {
            char ch[] = {wparam & 0xFF, '\0'};
            
            if (ch[0] == 0x02) {
                finalize_rename(handle, state);
                dwm_window_send_event(handle, DWM_EVENT_REDRAW);
                break;
            }
            
            uint16_t max_cols = (state->win_width - NAV_X) / ITEM_WIDTH;
            if (max_cols == 0) max_cols = 1;
            uint16_t col = state->context_item_index % max_cols;
            uint16_t row = state->context_item_index / max_cols;
            uint16_t item_x = NAV_X + (col * ITEM_WIDTH);
            uint16_t item_y = NAV_Y + (row * ITEM_HEIGHT) + 1;
            
            if (ch[0] == 0x01) {
                dwm_window_edit_backspace(handle, state->edit_handle);
                
                size_t current_len = dwm_window_edit_get_len(handle, state->edit_handle);
                uint16_t new_width = current_len * ITEM_FONT_CHAR_WIDTH;
                
                if (new_width < 30) new_width = 30; 
                state->edit_width = new_width;
                
                int16_t edit_x = item_x + ((ITEM_WIDTH - state->edit_width) / 2);
                
                dwm_window_edit_set_width(handle, state->edit_handle, state->edit_width);
                dwm_window_edit_set_pos(handle, state->edit_handle, edit_x, item_y + ITEM_TEXT_HEIGHT_OFF);
                
                dwm_window_send_event(handle, DWM_EVENT_REDRAW);
                break;
            }
            
            // Stop accepting characters once the name is at the limit the
            // file system can store
            if (dwm_window_edit_get_len(handle, state->edit_handle) >= VFS_NAME_MAX) {
                break;
            }
            
            dwm_window_edit_insert(handle, state->edit_handle, ch);
            
            size_t current_len = dwm_window_edit_get_len(handle, state->edit_handle);
            uint16_t new_width = current_len * ITEM_FONT_CHAR_WIDTH;
            
            if (new_width > MAX_RENAME_WIDTH) {
                new_width = MAX_RENAME_WIDTH;
            } else if (new_width < 30) {
                new_width = 30;
            }
            
            state->edit_width = new_width;
            
            int16_t edit_x = item_x + ((ITEM_WIDTH - state->edit_width) / 2);
            
            dwm_window_edit_set_width(handle, state->edit_handle, state->edit_width);
            dwm_window_edit_set_pos(handle, state->edit_handle, edit_x, item_y + ITEM_TEXT_HEIGHT_OFF);
            
            dwm_window_send_event(handle, DWM_EVENT_REDRAW);
        }
        break;
        
    case DWM_EVENT_RESIZE:
        handle_explorer_resize(state, wparam);
        break;
        
    case DWM_EVENT_REDRAW:
        handle_explorer_redraw(handle, state);
        break;
        
    case DWM_EVENT_DESTROY:
        free_window_state(handle);
        return;
        
    case DWM_EVENT_REFRESH:
        populate_state_from_vfs(state, state->full_path);
        dwm_window_send_event(handle, DWM_EVENT_REDRAW);
        break;
        
    case DWM_EVENT_CONTEXT_MENU:
        switch (state->context_directive) {
        case 0:  // Window context menu
            switch (wparam) {
            case 0:
                populate_state_from_vfs(state, state->full_path);
                dwm_window_send_event(handle, DWM_EVENT_REDRAW);
                break;
                
            case 1: { // New Folder
                char new_folder_path[128];
                generate_unique_name(state, new_folder_path, "new_folder");
                if (vfs_mkdir(new_folder_path))
                    dwm_notify_item_created(new_folder_path);
                populate_state_from_vfs(state, state->full_path);
                dwm_window_send_event(handle, DWM_EVENT_REDRAW);
                break;
            }
                
            case 2: { // New File
                char new_file_path[128];
                generate_unique_name(state, new_file_path, "new_file");
                
                File file = vfs_open(new_file_path, VFS_OPEN_CREATE);
                if (file != VFS_INVALID_FILE) {
                    vfs_close(file);
                    dwm_notify_item_created(new_file_path);
                }
                
                populate_state_from_vfs(state, state->full_path);
                dwm_window_send_event(handle, DWM_EVENT_REDRAW);
                break;
            }
            case 3: // Paste
                dwm_clipboard_paste_into(state->full_path, -1, -1);
                populate_state_from_vfs(state, state->full_path);
                dwm_window_send_event(handle, DWM_EVENT_REDRAW);
                state->context_item_index = -1;
                break;
            case 4:
                dwm_summon_properties("Properties", state->window_title, state->full_path, ICON_FOLDER);
                state->context_item_index = -1;
                break;
            }
            break;
            
        case 1:  // Item context menu
            switch (wparam) {
            case 0: { // Open
                struct Item* clicked_item = &state->items[state->context_item_index];
                if (vfs_directory_check(clicked_item->path)) {
                    populate_state_from_vfs(state, clicked_item->path);
                    dwm_window_send_event(handle, DWM_EVENT_REDRAW);
                } else if (dwm_path_is_executable(clicked_item->path)) {
                    dwm_launch_program(clicked_item->path);
                } else {
                    dwm_kernel_event_send(KEVENT_EXECUTE, "notepad", clicked_item->path);
                }
                state->context_item_index = -1;
                break;
            }
            case 1:   // Cut
            case 2: { // Copy
                struct Item* clicked_item = &state->items[state->context_item_index];
                dwm_clipboard_put_file(clicked_item->path,
                                       (wparam == 1) ? DWM_CLIPBOARD_OP_CUT : DWM_CLIPBOARD_OP_COPY);
                state->context_item_index = -1;
                break;
            }
            case 3: // Rename
                if (state->edit_handle != 0 && state->context_item_index != -1) {
                    uint16_t max_cols = (state->win_width - NAV_X) / ITEM_WIDTH;
                    if (max_cols == 0) max_cols = 1;
                    struct Item* clicked_item = &state->items[state->context_item_index];
    
                    uint16_t col = state->context_item_index % max_cols;
                    uint16_t row = state->context_item_index / max_cols;
                    
                    uint16_t item_x = NAV_X + (col * ITEM_WIDTH);
                    uint16_t item_y = NAV_Y + (row * ITEM_HEIGHT) + 1;
                    
                    size_t name_len = strlen(clicked_item->name);
                    uint16_t initial_width = name_len * ITEM_FONT_CHAR_WIDTH;
                    
                    if (initial_width < 30) initial_width = 30;
                    if (initial_width > MAX_RENAME_WIDTH) initial_width = MAX_RENAME_WIDTH;
                    
                    state->edit_width = initial_width;
                    dwm_window_edit_set_width(handle, state->edit_handle, state->edit_width);
                    
                    int16_t edit_x = item_x + ((ITEM_WIDTH - state->edit_width) / 2);
                    
                    dwm_window_edit_text(handle, state->edit_handle, clicked_item->name);
                    dwm_window_edit_set_pos(handle, state->edit_handle, edit_x, item_y + ITEM_TEXT_HEIGHT_OFF);
                    dwm_window_edit_visible(handle, state->edit_handle, true);
                    
                    clicked_item->name[0] = '\0';
                    dwm_window_send_event(handle, DWM_EVENT_REDRAW);
                }
                break;
            case 4: { // Delete
                struct Item* clicked_item = &state->items[state->context_item_index];
                dwm_summon_dialog_delete("Deletion request", clicked_item->path, handle, 1);
                state->context_item_index = -1;
                break;
            }
            case 5: { // Properties
                struct Item* clicked_item = &state->items[state->context_item_index];
                dwm_summon_properties("Properties", clicked_item->name, clicked_item->path, clicked_item->icon_index);
                state->context_item_index = -1;
                break;
            }
            }
            break;
        }
        return;
    }
}
