#include <kernel/dwm/windows/properties.h>
#include <kernel/dwm/dwm_core_internal.h>

#include <kernel/util/string.h>
#include <kernel/util/parser.h>
#include <kernel/util/tok.h>

#include <kernel/knode.h>
#include <kernel/malloc.h>

#include <kernel/vfs/vfs.h>
#include <kernel/events.h>

// ==========================================
// Size formatting
// ==========================================
//
//   0 .. 999 bytes     "512 (bytes)"
//   1,000 and up       "1.0 (kilobytes)", "2.5 (megabytes)", ...
//
// One decimal, rounded. Set PROP_SIZE_BASE to 1024 for binary units.
// All 64-bit math is shifts, adds and compares (no __udivdi3 on i386).

#define PROP_SIZE_BASE      1000u

static const char* const prop_size_units[] = {
    "bytes", "kilobytes", "megabytes", "gigabytes", "terabytes"
};
#define PROP_SIZE_UNIT_COUNT  (sizeof(prop_size_units) / sizeof(prop_size_units[0]))

// n / d by shift-and-subtract (d > 0, d < 2^63)
static uint64_t prop_div_u64(uint64_t n, uint64_t d) {
    if (d == 0) return 0;

    uint64_t q = 0;
    uint64_t r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) {
            r -= d;
            q |= ((uint64_t)1 << i);
        }
    }
    return q;
}

static size_t prop_u32_to_str(uint32_t value, char* out) {
    char digits[11];
    size_t n = 0;
    do {
        digits[n++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0);

    for (size_t i = 0; i < n; i++) out[i] = digits[n - 1 - i];
    out[n] = '\0';
    return n;
}

static void prop_format_size(uint64_t bytes, char* out, size_t size) {
    if (out == NULL || size == 0) return;
    memset(out, '\0', size);

    char num[12];

    if (bytes < PROP_SIZE_BASE) {
        prop_u32_to_str((uint32_t)bytes, num);
        strncpy(out, num, size - 1);
        strncat(out, (bytes == 1) ? " (byte)" : " (bytes)", size - strlen(out) - 1);
        return;
    }

    // Largest unit the value reaches
    uint32_t unit    = 0;
    uint64_t divisor = 1;
    while (unit + 1 < PROP_SIZE_UNIT_COUNT && bytes >= divisor * PROP_SIZE_BASE) {
        divisor *= PROP_SIZE_BASE;
        unit++;
    }

    // Tenths of that unit, rounded
    uint64_t tenths = prop_div_u64(bytes * 10 + (divisor >> 1), divisor);

    // 999,960 bytes rounds to "1000.0 kilobytes": show "1.0 megabytes"
    if (tenths >= (uint64_t)PROP_SIZE_BASE * 10 && unit + 1 < PROP_SIZE_UNIT_COUNT) {
        divisor *= PROP_SIZE_BASE;
        unit++;
        tenths = prop_div_u64(bytes * 10 + (divisor >> 1), divisor);
    }

    uint64_t whole = prop_div_u64(tenths, 10);
    uint32_t frac  = (uint32_t)(tenths - whole * 10);

    prop_u32_to_str((uint32_t)whole, num);
    strncpy(out, num, size - 1);

    char dot[3] = { '.', (char)('0' + frac), '\0' };
    strncat(out, dot, size - strlen(out) - 1);
    strncat(out, " (", size - strlen(out) - 1);
    strncat(out, prop_size_units[unit], size - strlen(out) - 1);
    strncat(out, ")", size - strlen(out) - 1);
}

static uint16_t get_knode_path_len(const char* file_path) {
    if (!file_path || file_path[0] != '/') {
        return file_path ? (uint16_t)strlen(file_path) : 0;
    }

    char path_scratch[DWM_MAX_PATH_LEN];
    strncpy(path_scratch, file_path, DWM_MAX_PATH_LEN - 1);
    path_scratch[DWM_MAX_PATH_LEN - 1] = '\0';

    uint32_t current_knode = knode_get_root();
    uint16_t knode_path_len = (uint16_t)strlen(file_path);
    uint16_t current_pos = 0;

    cstr_tok_t tok;
    cstr_tok_init(&tok, path_scratch, "/");

    char* token = cstr_tok_next(&tok);
    while (token != NULL) {
        if (current_knode == KNODE_NULL || current_knode == 0) {
            break;
        }

        uint32_t found_node = knode_find_by_name(current_knode, token);
        if (found_node != KNODE_NULL && found_node != 0) {
            uint8_t flags = kmalloc_get_flags(found_node);
            if ((flags & KMALLOC_FLAG_DIRECTORY) && (flags & KMALLOC_FLAG_MOUNT)) {
                // Found the partition mount point; cut off the knode prefix here
                knode_path_len = (current_pos == 0) ? 1 : current_pos;
                break;
            }
            current_knode = found_node;
            current_pos += 1 + (uint16_t)strlen(token);
        } else {
            break;
        }
        token = cstr_tok_next(&tok);
    }

    return knode_path_len;
}

WindowHandle dwm_summon_properties(const char* title, const char* name, const char* file_path, uint16_t icon_index) {
    DWM_GUARD();
    WindowClass wclass_props;
    uint16_t width  = 300;
    uint16_t height = 360;
    
    // Center the message box on the screen
    wclass_props.width  = width;
    wclass_props.height = height;
    wclass_props.x      = (display_get_width() - width) / 2;
    wclass_props.y      = (display_get_height() - height) / 2;
    
    // Assign boundaries limits
    wclass_props.max_width  = width;
    wclass_props.max_height = height;
    
    // Copy the title over to the class context
    strncpy(wclass_props.title, title, DWM_MAX_TITLE_LEN - 1);
    wclass_props.title[DWM_MAX_TITLE_LEN - 1] = '\0';
    
    struct WindowObject* msg_handle = dwm_allocate_window(
        wclass_props, 
        0, 
        (WindowProcedure)callback_properties_handler
    );
    if (msg_handle == NULL) return 0;
    
    // Instance metadata TAG
    char* instance = (char*)malloc(16);
    if (instance != NULL) {
        memset(instance, 0, 16);
        dwm_window_resource_add(msg_handle->id, "instance", instance);
    }
    
    // General - file metadata
    {
        size_t path_length = strnlen(file_path, DWM_MAX_PATH_LEN);
        size_t name_length = strnlen(name, DWM_MAX_PATH_LEN);
        
        char* target_path = (char*)malloc(DWM_MAX_PATH_LEN);
        char* target_name = (char*)malloc(DWM_MAX_PATH_LEN);
        char* target_type = (char*)malloc(DWM_MAX_PATH_LEN);
        char* target_size = (char*)malloc(DWM_MAX_PATH_LEN);
        
        // Zero out allocated memory to prevent garbage strings
        memset(target_path, 0, DWM_MAX_PATH_LEN);
        memset(target_name, 0, DWM_MAX_PATH_LEN);
        memset(target_type, 0, DWM_MAX_PATH_LEN);
        memset(target_size, 0, DWM_MAX_PATH_LEN);
        
        uint32_t size = 0;
        
        File file = vfs_open(file_path, VFS_OPEN_READ);
        if (file != VFS_INVALID_FILE) {
            size = vfs_get_size(file);
            vfs_close(file);
        }
        
        uint16_t type_index = 0;
        if (vfs_directory_check(file_path)) {
            if (vfs_directory_check_mounted(file_path)) {
                type_index = 1;
                strncpy(target_type, "Storage", DWM_MAX_PATH_LEN - 1);
            } else {
                type_index = 2;
                strncpy(target_type, "Folder", DWM_MAX_PATH_LEN - 1);
            }
        } else {
            switch (icon_index) {
                case 1: 
                    type_index = 3; 
                    strncpy(target_type, "File", DWM_MAX_PATH_LEN - 1); 
                    break;
                case 2: 
                    type_index = 4; 
                    strncpy(target_type, "Document", DWM_MAX_PATH_LEN - 1); 
                    break;
                case 3: 
                    type_index = 5; 
                    strncpy(target_type, "System", DWM_MAX_PATH_LEN - 1); 
                    break;
                default: 
                    type_index = 3; 
                    strncpy(target_type, "File", DWM_MAX_PATH_LEN - 1); 
                    break;
            }
        }
        
        switch (type_index) {
            default: // General file types
                prop_format_size(size, target_size, DWM_MAX_PATH_LEN);
                break;
                
            case 1: { // Storage
                char* target_used = (char*)malloc(DWM_MAX_PATH_LEN);
                char* target_free = (char*)malloc(DWM_MAX_PATH_LEN);
                char* target_total = (char*)malloc(DWM_MAX_PATH_LEN);
                
                memset(target_used, 0, DWM_MAX_PATH_LEN);
                memset(target_free, 0, DWM_MAX_PATH_LEN);
                memset(target_total, 0, DWM_MAX_PATH_LEN);
                
                uint32_t used = vfs_device_get_used(file_path);
                uint32_t total = vfs_device_get_capacity(file_path);
                uint32_t free = total - used;
                
                itos_commas(used, target_used);
                itos_commas(free, target_free);
                itos_commas(total, target_total);
                
                dwm_window_resource_add(msg_handle->id, "used", target_used);
                dwm_window_resource_add(msg_handle->id, "free", target_free);
                dwm_window_resource_add(msg_handle->id, "total", target_total);
                break;
            }
            case 2: { // Folder: direct item count plus the size of everything inside
                char* target_items = (char*)malloc(DWM_MAX_PATH_LEN);
                if (target_items != NULL) {
                    memset(target_items, 0, DWM_MAX_PATH_LEN);
                    size_t item_count = vfs_directory_get_item_count(file_path);
                    itos_commas((uint32_t)item_count, target_items);
                    dwm_window_resource_add(msg_handle->id, "items", target_items);
                }
                
                VfsDirectorySize dir_size;
                if (vfs_directory_get_size(file_path, &dir_size)) {
                    prop_format_size(dir_size.bytes, target_size, DWM_MAX_PATH_LEN);
                    
                    // Something inside was skipped: say so under the size
                    if (!dir_size.complete) {
                        char* partial = (char*)malloc(1);
                        if (partial != NULL) {
                            partial[0] = 1;
                            dwm_window_resource_add(msg_handle->id, "partial", partial);
                        }
                    }
                } else {
                    strncpy(target_size, "Unknown", DWM_MAX_PATH_LEN - 1);
                }
                break;
            }
        }
        
        // File name
        strncpy(target_name, name, name_length);
        target_name[name_length] = '\0';
        
        strncpy(target_path, file_path, path_length);
        target_path[path_length] = '\0';
        
        dwm_window_resource_add(msg_handle->id, "path", target_path);
        dwm_window_resource_add(msg_handle->id, "name", target_name);
        dwm_window_resource_add(msg_handle->id, "type", target_type);
        dwm_window_resource_add(msg_handle->id, "size", target_size);
    }
    
    // Attributes
    {
        char* target_attrib = (char*)malloc(16);
        if (target_attrib != NULL) {
            memset(target_attrib, ' ', 16);
            
            uint8_t permissions = 0;
            vfs_get_permissions(file_path, &permissions);
            
            if (permissions & VFS_PERMISSION_EXECUTE) target_attrib[0] = 'x';
            if (permissions & VFS_PERMISSION_READ)    target_attrib[1] = 'r';
            if (permissions & VFS_PERMISSION_WRITE)   target_attrib[2] = 'w';
            
            dwm_window_resource_add(msg_handle->id, "perms", target_attrib);
        }
    }
    
    dwm_set_focus(msg_handle);
    return msg_handle->id;
}

void callback_properties_handler(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam) {
    struct WindowObject* window = dwm_get_window_by_id(handle);
    char* instance = dwm_window_resource_get_by_name(handle, "instance");
    
    if (window == NULL) return;
    
    // ==========================================
    // LAYOUT & POSITIONING CONFIGURATION
    // ==========================================
    uint8_t current_tab = (instance != NULL) ? (uint8_t)instance[0] : 0;
    
    // Tab Layout (Adjusted gap/offsets for 3 tabs inside 300px width)
    uint16_t tab_width  = 76; 
    uint16_t tab_height = 24;
    uint16_t tab_gap    = 4; 
    
    uint16_t tab_general_x = 30; 
    uint16_t tab_general_y = 5;
    
    uint16_t tab_perm_x = tab_general_x + tab_width + tab_gap;
    uint16_t tab_perm_y = 5;

    uint16_t tab_security_x = tab_perm_x + tab_width + tab_gap;
    uint16_t tab_security_y = 5;
    
    // General Tab Layout Configurations
    uint16_t icon_x      = 24;
    uint16_t icon_y      = 54;
    
    // Permissions Tab Element Layout
    uint16_t box_size    = 12;
    uint16_t box_x       = 25;
    uint16_t label_x     = 45;
    uint16_t read_box_y  = 60;
    uint16_t write_box_y = 85;
    uint16_t exec_box_y  = 110; 
    
    // Close Button Layout
    uint16_t button_width  = 60;
    uint16_t button_height = 24;
    uint16_t button_x = window->w - (button_width + (button_width / 2));
    uint16_t button_y = ((window->h - button_height) - button_height) - 7;
    
    // Color Configurations
    uint32_t color_bg            = theme.client.background;
    uint32_t color_divider       = theme.client.accent;
    uint32_t color_text_primary  = theme.client.text;
    uint32_t color_text_value    = theme.client.text_value;
    uint32_t color_text_mount    = theme.client.text_mount;
    uint32_t color_text_inactive = theme.client.text_muted;
    uint32_t color_border_active = theme.button.border_active;
    uint32_t color_border_normal = theme.button.border;
    uint32_t color_fill_active   = theme.client.accent;
    uint32_t color_fill_button   = theme.button.fill;
    
    switch (event) {
            
        case DWM_EVENT_KEYBOARD:
            if (wparam == 0x1B) 
                dwm_window_send_event(handle, DWM_EVENT_CLOSE);
            break;
            
        case DWM_EVENT_REDRAW:
            // Window background
            dwm_draw_rect_filled(0, 0, window->w, window->h, color_bg);
            dwm_draw_line(0, 35, window->w, 0, color_divider);
            
            // General tab
            if (current_tab == 0) {
                char* path_str = (char*)dwm_window_resource_get_by_name(handle, "path");
                dwm_draw_text(15, 110, "Path", color_text_primary);
                if (path_str != NULL) {
                    uint16_t path_len = (uint16_t)strlen(path_str);
                    uint16_t knode_len = get_knode_path_len(path_str);

                    if (knode_len >= path_len) {
                        dwm_draw_text(15, 124, path_str, color_text_value);
                    } else {
                        char base_buffer[DWM_MAX_PATH_LEN];
                        memset(base_buffer, 0, DWM_MAX_PATH_LEN);
                        strncpy(base_buffer, path_str, knode_len);

                        dwm_draw_text(15, 124, base_buffer, color_text_value);
                        int16_t mount_offset_x = 15 + (knode_len * 6); // 6px character font offset
                        dwm_draw_text(mount_offset_x, 124, path_str + knode_len, color_text_mount);
                    }
                }
                
                char* name_str = (char*)dwm_window_resource_get_by_name(handle, "name");
                if (name_str != NULL) dwm_draw_text(90, 78, name_str, color_text_value);
                
                uint16_t type_index = 0;
                char* type_str = (char*)dwm_window_resource_get_by_name(handle, "type");
                
                if (type_str != NULL) {
                    dwm_draw_text(90, 58, type_str, color_text_primary);
                    if (strncmp(type_str, "File",     5) == 0) {type_index = 1; dwm_draw_sprite(icon_x, icon_y, (struct Image*)dwm_resource_find("icon_file"));}
                    else if (strncmp(type_str, "Folder",   7) == 0) {type_index = 2; dwm_draw_sprite(icon_x, icon_y, (struct Image*)dwm_resource_find("icon_folder"));}
                    else if (strncmp(type_str, "Document", 9) == 0) {type_index = 3; dwm_draw_sprite(icon_x, icon_y, (struct Image*)dwm_resource_find("icon_document"));}
                    else if (strncmp(type_str, "System",   7) == 0) {type_index = 4; dwm_draw_sprite(icon_x, icon_y, (struct Image*)dwm_resource_find("icon_system"));}
                    else if (strncmp(type_str, "Storage",  8) == 0) {type_index = 5; dwm_draw_sprite(icon_x, icon_y, (struct Image*)dwm_resource_find("icon_storage"));}
                }
                
                if (type_index == 2) { 
                    char* items_str = (char*)dwm_window_resource_get_by_name(handle, "items");
                    char* size_str  = (char*)dwm_window_resource_get_by_name(handle, "size");
                    
                    dwm_draw_text(15, 150, "Items", color_text_primary);
                    if (items_str != NULL) dwm_draw_text(90, 150, items_str, color_text_value);
                    
                    dwm_draw_text(15, 165, "Size", color_text_primary);
                    if (size_str != NULL) dwm_draw_text(90, 165, size_str, color_text_value);
                    
                    if (dwm_window_resource_get_by_name(handle, "partial") != NULL)
                        dwm_draw_text(90, 180, "(not every item counted)", color_text_inactive);
                } else if (type_index == 5) { 
                    char* used_str = (char*)dwm_window_resource_get_by_name(handle, "used");
                    char* free_str = (char*)dwm_window_resource_get_by_name(handle, "free");
                    char* total_str = (char*)dwm_window_resource_get_by_name(handle, "total");
                    
                    dwm_draw_text(15, 150, "Used", color_text_primary);
                    dwm_draw_text(15, 165, "Free", color_text_primary);
                    dwm_draw_text(15, 180, "Total", color_text_primary);
                    
                    if (used_str != NULL) dwm_draw_text(90, 150, used_str, color_text_value);
                    if (free_str != NULL) dwm_draw_text(90, 165, free_str, color_text_value);
                    if (total_str != NULL) dwm_draw_text(90, 180, total_str, color_text_value);
                    
                    // Draw usage pie
                    uint32_t used  = stoi_commas(used_str);
                    uint32_t total = stoi_commas(total_str);
                    
                    // Prevent division by zero if total space is reported as 0
                    if (total == 0) 
                        total = 1;
                    
                    // Calculate used percentage using 64-bit arithmetic to prevent overflow
                    uint32_t used_percent = (uint32_t)(((uint64_t)used * 100) / total);
                    
                    // Ensure it doesn't accidentally exceed 100 due to rounding or weird data
                    if (used_percent > 100) {
                        used_percent = 100;
                    }
                    
                    uint32_t free_percent = 100 - used_percent;
                    
                    // Draw usage pie dynamically
                    uint32_t values[] = { used_percent, free_percent };
                    uint32_t colors[] = { theme.chart.used, theme.chart.available };
                    
                    draw_pie_chart_isometric(150, 250, 45, 10, values, colors, sizeof(values) / sizeof(uint32_t));
                } else { 
                    char* size_str = (char*)dwm_window_resource_get_by_name(handle, "size");
                    dwm_draw_text(15, 150, "Size", color_text_primary);
                    if (size_str != NULL) dwm_draw_text(90, 150, size_str, color_text_value);
                }
            } 
            
            // Permissions tab
            else if (current_tab == 1) {
                char* attrib_str = (char*)dwm_window_resource_get_by_name(handle, "perms");
                
                if (attrib_str != NULL) {
                    int is_executable = 0;
                    int is_readable   = 0;
                    int is_writable   = 0;
                    
                    if (attrib_str[0] == 'x') is_executable = 1;
                    if (attrib_str[1] == 'r') is_readable = 1;
                    if (attrib_str[2] == 'w') is_writable = 1;
                    
                    dwm_draw_rect(box_x, read_box_y, box_size, box_size, color_border_normal);
                    if (is_readable) {
                        dwm_draw_rect_filled(box_x + 2, read_box_y + 2, box_size - 4, box_size - 4, color_fill_active);
                    }
                    dwm_draw_text(label_x, read_box_y + 2, "Readable", color_text_primary);
                    
                    dwm_draw_rect(box_x, write_box_y, box_size, box_size, color_border_normal);
                    if (is_writable) {
                        dwm_draw_rect_filled(box_x + 2, write_box_y + 2, box_size - 4, box_size - 4, color_fill_active);
                    }
                    dwm_draw_text(label_x, write_box_y + 2, "Writeable", color_text_primary);
                    
                    dwm_draw_rect(box_x, exec_box_y, box_size, box_size, color_border_normal);
                    if (is_executable) {
                        dwm_draw_rect_filled(box_x + 2, exec_box_y + 2, box_size - 4, box_size - 4, color_fill_active);
                    }
                    dwm_draw_text(label_x, exec_box_y + 2, "Executable", color_text_primary);
                }
            }
            
            // Security tab
            else if (current_tab == 2) {
                dwm_draw_text(15, 60, "Advanced Security Settings", color_text_primary);
            }
            
            // ------------------------------------------
            // TAB BUTTON RENDERING
            // ------------------------------------------
            
            // General Tab Button
            {
                int is_active = (current_tab == 0);
                uint32_t border_color = is_active ? color_border_active : color_border_normal; 
                uint32_t text_color   = is_active ? color_text_primary : color_text_inactive;
                
                dwm_draw_rect_filled(tab_general_x, tab_general_y, tab_width, tab_height, color_fill_button);
                dwm_draw_rect(tab_general_x, tab_general_y, tab_width, tab_height, border_color);
                
                const char* text = "General"; 
                dwm_draw_text(tab_general_x + ((tab_width - (strlen(text) * 6)) / 2), tab_general_y + 8, text, text_color);
            }
            
            // Permissions Tab Button
            {
                int is_active = (current_tab == 1);
                uint32_t border_color = is_active ? color_border_active : color_border_normal;
                uint32_t text_color   = is_active ? color_text_primary : color_text_inactive;
                
                dwm_draw_rect_filled(tab_perm_x, tab_perm_y, tab_width, tab_height, color_fill_button);
                dwm_draw_rect(tab_perm_x, tab_perm_y, tab_width, tab_height, border_color);
                
                const char* text = "Permissions";
                dwm_draw_text(tab_perm_x + ((tab_width - (strlen(text) * 6)) / 2), tab_perm_y + 8, text, text_color);
            }

            // Security Tab Button
            {
                int is_active = (current_tab == 2);
                uint32_t border_color = is_active ? color_border_active : color_border_normal;
                uint32_t text_color   = is_active ? color_text_primary : color_text_inactive;
                
                dwm_draw_rect_filled(tab_security_x, tab_security_y, tab_width, tab_height, color_fill_button);
                dwm_draw_rect(tab_security_x, tab_security_y, tab_width, tab_height, border_color);
                
                const char* text = "Security"; 
                dwm_draw_text(tab_security_x + ((tab_width - (strlen(text) * 6)) / 2), tab_security_y + 8, text, text_color);
            }
            
            // Close Button
            {
                dwm_draw_rect_filled(button_x, button_y, button_width, button_height, color_fill_button);
                dwm_draw_rect(button_x, button_y, button_width, button_height, color_border_normal);
                dwm_draw_text(button_x + ((button_width - 30) / 2), button_y + 8, "Close", color_text_value);
            }
            break;
            
        case DWM_EVENT_MOUSE: {
            if (lparam & DWM_STATE_MOUSE_BTN_RIGHT) 
                break;
            
            uint16_t click_x = (uint16_t)(wparam & 0xFFFF);
            uint16_t click_y = (uint16_t)((wparam >> 16) & 0xFFFF);
            int state_changed = 0;
            
            if (instance != NULL) {
                // General Tab Click Bounds
                if (click_x >= tab_general_x && click_x < (tab_general_x + tab_width) && 
                    click_y >= tab_general_y && click_y < (tab_general_y + tab_height)) {
                    if (instance[0] != 0) {
                        instance[0] = 0;
                        state_changed = 1;
                    }
                }
                // Permissions Tab Click Bounds
                else if (click_x >= tab_perm_x && click_x < (tab_perm_x + tab_width) && 
                         click_y >= tab_perm_y && click_y < (tab_perm_y + tab_height)) {
                    if (instance[0] != 1) {
                        instance[0] = 1;
                        state_changed = 1;
                    }
                }
                // Security Tab Click Bounds
                else if (click_x >= tab_security_x && click_x < (tab_security_x + tab_width) && 
                         click_y >= tab_security_y && click_y < (tab_security_y + tab_height)) {
                    if (instance[0] != 2) {
                        instance[0] = 2;
                        state_changed = 1;
                    }
                }
                
                // Active checkbox processing inside permissions tab
                if (current_tab == 1) {
                    char* attrib_str = (char*)dwm_window_resource_get_by_name(handle, "perms");
                    char* path_str = (char*)dwm_window_resource_get_by_name(handle, "path");
                    
                    if (attrib_str && path_str) {
                        // Read checkbox
                        if (click_x >= box_x && click_x < (box_x + box_size) &&
                            click_y >= read_box_y && click_y < (read_box_y + box_size)) {
                            
                            char parent_path[DWM_MAX_PATH_LEN];
                            parse_get_parent_path(path_str, parent_path, DWM_MAX_PATH_LEN);
                            
                            uint8_t permissions;
                            vfs_get_permissions(parent_path, &permissions);
                            
                            if (permissions & VFS_PERMISSION_WRITE) {
                                attrib_str[1] = (attrib_str[1] == 'r') ? ' ' : 'r';
                                state_changed = 1;
                                
                                uint8_t perm = 0;
                                vfs_get_permissions(path_str, &perm);
                                
                                if (attrib_str[1] == 'r') {
                                    perm |= VFS_PERMISSION_READ;
                                } else {
                                    perm &= ~VFS_PERMISSION_READ;
                                }
                                vfs_set_permissions(path_str, perm);
                            }
                        }
                        // Write checkbox
                        else if (click_x >= box_x && click_x < (box_x + box_size) &&
                                 click_y >= write_box_y && click_y < (write_box_y + box_size)) {
                            
                            char parent_path[DWM_MAX_PATH_LEN];
                            parse_get_parent_path(path_str, parent_path, DWM_MAX_PATH_LEN);
                            
                            uint8_t permissions;
                            vfs_get_permissions(parent_path, &permissions);
                            
                            if (permissions & VFS_PERMISSION_WRITE) {
                                attrib_str[2] = (attrib_str[2] == 'w') ? ' ' : 'w';
                                state_changed = 1;
                                
                                uint8_t perm = 0;
                                vfs_get_permissions(path_str, &perm);
                                
                                if (attrib_str[2] == 'w') {
                                    perm |= VFS_PERMISSION_WRITE;
                                } else {
                                    perm &= ~VFS_PERMISSION_WRITE;
                                }
                                vfs_set_permissions(path_str, perm);
                            }
                        }
                        // Execute checkbox
                        else if (click_x >= box_x && click_x < (box_x + box_size) &&
                                 click_y >= exec_box_y && click_y < (exec_box_y + box_size)) {
                            
                            char parent_path[DWM_MAX_PATH_LEN];
                            parse_get_parent_path(path_str, parent_path, DWM_MAX_PATH_LEN);
                            
                            uint8_t permissions;
                            vfs_get_permissions(parent_path, &permissions);
                            
                            if (permissions & VFS_PERMISSION_WRITE) {
                                attrib_str[0] = (attrib_str[0] == 'x') ? ' ' : 'x';
                                state_changed = 1;
                                
                                uint8_t perm = 0;
                                vfs_get_permissions(path_str, &perm);
                                
                                if (attrib_str[0] == 'x') {
                                    perm |= VFS_PERMISSION_EXECUTE;
                                } else {
                                    perm &= ~VFS_PERMISSION_EXECUTE;
                                }
                                vfs_set_permissions(path_str, perm);
                            }
                        }
                    }
                }
            }
            
            // Close Button Click Bounds
            if (click_x >= button_x && click_x < (button_x + button_width) && 
                click_y >= button_y && click_y < (button_y + button_height)) {
                dwm_window_send_event(handle, DWM_EVENT_CLOSE);
            }
            
            if (state_changed) {
                dwm_window_send_event(handle, DWM_EVENT_REDRAW);
            }
            break;
        }
        default: 
            break;
    }
}
