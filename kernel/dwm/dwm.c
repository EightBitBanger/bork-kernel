#include <kernel/dwm/dwm.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/dwm/dwm_core_internal.h>
#include <kernel/dwm/dwm_context_menu.h>

#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/resources/classes/image.h>

#include <kernel/console/mouse.h>
#include <kernel/console/display.h>

#include <kernel/util/string.h>
#include <kernel/util/list.h>
#include <kernel/util/map.h>

#include <kernel/scheduler/watchdog.h>

struct DWMWorkspace    workspace;
struct DWMContext      context;
struct DWMTaskbar      taskbar;
struct DWMDragDrop     dragdrop;
struct DWMInput        input;
struct DWMContextMenu  ctxmenu;
struct DWMImages       images;
struct DWMCascade      cascade;

// ==========================================
// Image resources
// ==========================================

const struct DWMBuiltinImage dwm_builtin_images[] = {
    
#ifdef ADD_IMAGE_LIB
    
    { "icon_folder",     &rc_icon_folder         },
    { "icon_file",       &rc_icon_file           },
    { "icon_document",   &rc_icon_document       },
    { "icon_system",     &rc_icon_system         },
    { "icon_storage",    &rc_icon_storage        },
    { "image_error",     &rc_image_error         },
    { "ui_close",        &rc_button_close        },
    { "ui_close_red",    &rc_button_close_red    },
    { "ui_close_purple", &rc_button_close_purple },
    { "ui_minimize",     &rc_button_minimize     },
    { "ui_back",         &rc_button_back         },
    { "ui_new",          &rc_button_plus         },
    { "ui_button",       &rc_button              },
    { "ui_power",        &rc_button_power        },
    { "ui_start",        &rc_button_new          },
    { "cur_edge",        &rc_cursor_edge         },
    { "cur_pointer",     &rc_cursor_pointer      },
    { "cur_angle",       &rc_cursor_angle        },
    
#endif
    
};

const uint32_t dwm_builtin_image_count = sizeof(dwm_builtin_images) / sizeof(dwm_builtin_images[0]);

bool dwm_get_image_directory(char* out, size_t size) {
    if (out == NULL || size == 0) return false;
    
    struct LocalPaths paths;
    kernel_get_local_paths(&paths);
    
    // No home device was found
    if (paths.home[0] == '\0') return false;
    
    memset(out, '\0', size);
    strncpy(out, paths.home, size - 1);
    strncat(out, "/sys/images", size - strlen(out) - 1);
    return true;
}

// Load one sprite resource file (RSRC header + sprite payload, as written by
// resource_sprite_save) and register it under `name`.
static bool dwm_resource_image_load_file(const char* name, const char* path) {
    struct Image* img = resource_image_load(path);
    if (img == NULL) return false;

    dwm_resource_load(name, img);
    return true;
}

// Load DWM images in two passes:
//
//   1. Every sprite file in <home>/sys/images, registered under its file name.
//      An image does not need an entry in dwm_builtin_images to be loaded:
//      the file "icon_folder" becomes the resource "icon_folder".
//   2. Any built-in image whose name is still missing (no home device, file
//      absent or unreadable) is loaded from the compiled-in sprite.
static void dwm_load_images(void) {
    char image_dir[DWM_MAX_PATH_LEN];
    
    if (dwm_get_image_directory(image_dir, sizeof(image_dir)) &&
        vfs_directory_check(image_dir)) {
        
        uint32_t item_count = vfs_directory_get_item_count(image_dir);
        for (uint32_t i = 0; i < item_count; i++) {
            char item_name[DWM_MAX_PATH_LEN];
            memset(item_name, '\0', sizeof(item_name));
            if (!vfs_directory_get_item(image_dir, i, item_name, DWM_MAX_PATH_LEN))
                continue;
            
            // Already loaded (duplicate listing)
            if (dwm_resource_find(item_name) != NULL)
                continue;
            
            char image_path[DWM_MAX_PATH_LEN];
            memset(image_path, '\0', sizeof(image_path));
            strncpy(image_path, image_dir, sizeof(image_path) - 1);
            strncat(image_path, "/", sizeof(image_path) - strlen(image_path) - 1);
            strncat(image_path, item_name, sizeof(image_path) - strlen(image_path) - 1);
            
            if (vfs_directory_check(image_path))
                continue;
            
            // Non-sprite files fail the header check and are skipped
            dwm_resource_image_load_file(item_name, image_path);
        }
    }
    
    for (uint32_t i = 0; i < dwm_builtin_image_count; i++) {
        const struct DWMBuiltinImage* image = &dwm_builtin_images[i];
        if (dwm_resource_find(image->name) == NULL)
            dwm_resource_sprite_load(image->name, image->sprite);
    }
}

void dwm_initiate(void) {
    mutex_init(&dwm_mutex);
    dwm_clipboard_init();
    
    dwm_context_menu_init();
    
    // Named locks show up in watchdog reports (owner, waiters, cycles)
    wd_register_lock("dwm", &dwm_mutex);
    
    DWM_GUARD();
    
    // The compositor's drawing context on the display back buffer
    dwm_graphics_init();
    
    // Theme (must come before any window is created)
    dwm_theme_init();
    
    // Workspace
    workspace.next_edit_field_id = 0;
    workspace.next_window_id = 0;
    
    workspace.window_head = NULL;
    workspace.window_tail = NULL;
    
    workspace.desktop_focused = false;
    workspace.last_focus = 0;
    
    Point display_center;
    display_center.x = display_get_width() / 2;
    display_center.y = display_get_height() / 2;
    
    // Context
    context.event_window = NULL;
    context.focused_icon = NULL;
    context.last_focused_icon = NULL;
    
    context.last_icon_click_time = 0;
    memset(&context.window_context, 0x00, sizeof(struct WindowContext));
    context.window_context.cursor_width = 0;
    context.window_context.cursor_height  = 0;
    context.window_context.mouse = display_center;
    context.window_context.dirty_count = 0;
    
    // Taskbar
    taskbar.height = 28;
    
    WindowClass wclass_taskbar;
    wclass_taskbar.x = 0;
    wclass_taskbar.y = display_get_height() - taskbar.height;
    wclass_taskbar.width  = display_get_width();
    wclass_taskbar.height = taskbar.height;
    taskbar.window = dwm_create_window(wclass_taskbar, DWM_WSTYLE_TOPMOST | DWM_WSTYLE_NOBORDERS | DWM_WSTYLE_NOCLOSEBOX, callback_taskbar_handler);
    
    // Drag and drop
    dragdrop.dragged_window = NULL;
    dragdrop.drag_offset_x = 0;
    dragdrop.drag_offset_y = 0;
    dragdrop.dragged_icon = NULL;
    dragdrop.icon_drag_offset_x = 0;
    dragdrop.icon_drag_offset_y = 0;
    dragdrop.dragged_resizing = NULL;
    dragdrop.resize_offset_x = 0;
    dragdrop.resize_offset_y = 0;
    dragdrop.captured_window = 0;
    
    // Input
    input.last_key_pressed = 0;
    
    mouse_set_position(display_center.x, display_center.y);
    input.mouse_last = display_center;
    input.last_left_button_pressed = false;
    input.last_right_button_pressed = false;
    
    // Context menu
    ctxmenu.menu_count = 0;
    ctxmenu.menu_directive = 0;
    ctxmenu.handle = NULL;
    
    // Cascade
    uint16_t cascade_mul = 3;
    cascade.h = 17;
    cascade.w = 20;
    cascade.x = cascade.h * cascade_mul;
    cascade.y = cascade.w * cascade_mul;
    cascade.max = 400;
    
    // Load image resources from <home>/sys/images (built-in fallback)
    dwm_load_images();

    for(int i = 0; i < MAX_CONTEXT_MENUS; i++) {
        ctxmenu.menus[i].visible = false;
        ctxmenu.menus[i].x = 0;
        ctxmenu.menus[i].y = 0;
        ctxmenu.menus[i].w = 0;
        ctxmenu.menus[i].h = 0;
        ctxmenu.menus[i].color_bg         = theme.menu.background;
        ctxmenu.menus[i].color_border     = theme.menu.border;
        ctxmenu.menus[i].color_separator  = theme.menu.separator;
        ctxmenu.menus[i].color_highlight  = theme.menu.highlight;
        ctxmenu.menus[i].color_text       = theme.menu.text;
        ctxmenu.menus[i].hovered_item     = -1;
        ctxmenu.menus[i].item_height      = 0;
        ctxmenu.menus[i].item_count       = 0;
    }
    
    struct Image* def_cursor = dwm_resource_find("cur_pointer");
    if (def_cursor) {
        dwm_set_cursor(def_cursor->data, def_cursor->width, def_cursor->height);
    }
    
    GfxContext* gc = dwm_screen_context();
    gfx_clear(gc, theme.desktop.background);
    gfx_display_present_all();
}

WindowHandle dwm_create_window(WindowClass wclass, uint16_t wstyle, WindowProcedure wproc) {
    DWM_GUARD();
    struct WindowObject* window = dwm_allocate_window(wclass, wstyle, wproc);
    return window ? window->id : 0;
}

static void dwm_destroy_window_internal(WindowHandle handle) {
    if (handle == 0) return;
    
    struct WindowObject* window_handle = NULL;
    for (struct list_node* node = workspace.window_head; node != NULL; node = node->next) {
        struct WindowObject* win = (struct WindowObject*)node->data;
        if (win->id == handle) {
            window_handle = win;
            break;
        }
    }
    if (window_handle == NULL) return;

    dwm_window_resource_free_all(window_handle->id);
    
    if (dragdrop.dragged_window == window_handle) dragdrop.dragged_window = NULL;
    if (dragdrop.dragged_resizing == window_handle) dragdrop.dragged_resizing = NULL;
    if (dragdrop.captured_window == window_handle->id) dragdrop.captured_window = 0;
    
    int abs_x, abs_y;
    dwm_get_absolute_position(window_handle, &abs_x, &abs_y);
    
    int destroyed_min_x = abs_x - window_handle->border_width;
    int destroyed_max_x = abs_x + window_handle->w + window_handle->border_width;
    int destroyed_min_y = abs_y - window_handle->border_width;
    int destroyed_max_y = abs_y + window_handle->h + window_handle->border_width;
    
    list_remove(&workspace.window_head, &workspace.window_tail, window_handle);
    if (window_handle->parent != NULL) {
        list_remove(&window_handle->parent->children_head, 
                    &window_handle->parent->children_tail, 
                    window_handle);
        window_handle->parent = NULL;
    }
    
    while (window_handle->buttons_head != NULL) {
        struct list_node* btn_node = window_handle->buttons_head;
        struct WindowButton* btn = (struct WindowButton*)btn_node->data;
        list_remove(&window_handle->buttons_head, &window_handle->buttons_tail, btn);
        free(btn);
    }
    
    while (window_handle->edit_head != NULL) {
        struct list_node* edit_node = window_handle->edit_head;
        struct WindowEditField* editable = (struct WindowEditField*)edit_node->data;
        list_remove(&window_handle->edit_head, &window_handle->edit_tail, editable);
        free(editable);
    }
    
    while (window_handle->children_head != NULL) {
        struct list_node* child_node = window_handle->children_head;
        struct WindowObject* child = (struct WindowObject*)child_node->data;
        
        list_remove(&window_handle->children_head, &window_handle->children_tail, child);
        child->parent = NULL; 
        
        dwm_destroy_window_internal(child->id);
    }
    
    dwm_invalidate_region(destroyed_min_x, destroyed_min_y, 
                          destroyed_max_x - destroyed_min_x, 
                          destroyed_max_y - destroyed_min_y);

    if (workspace.window_tail != NULL) {
        struct WindowObject* tail_win = (struct WindowObject*)workspace.window_tail->data;
        // If the desktop holds focus (e.g. a popup closed because the desktop
        // was clicked) leave it there instead of refocusing the next window
        if (!workspace.desktop_focused)
            dwm_set_focus(tail_win); 
        tail_win->flags |= (DWM_WFLAG_REFRESH | DWM_WFLAG_REDECORATE);
        
        int tail_abs_x, tail_abs_y;
        dwm_get_absolute_position(tail_win, &tail_abs_x, &tail_abs_y);
        dwm_invalidate_region(tail_abs_x - tail_win->border_width,
                              tail_abs_y - tail_win->border_width,
                              tail_win->w + (tail_win->border_width * 2),
                              tail_win->h + (tail_win->border_width * 2));
    }
    
    if (window_handle->frame_buffer != NULL) 
        free(window_handle->frame_buffer);
    free(window_handle);
}

void dwm_destroy_window(WindowHandle handle) {
    DWM_GUARD();
    dwm_destroy_window_internal(handle);
}

struct IconObject* dwm_create_icon(uint16_t x, uint16_t y, uint16_t width, uint16_t height, struct Image* sprite, uint16_t icon_index) {
    DWM_GUARD();
    
    struct IconObject* icon = malloc(sizeof(struct IconObject));
    if (icon == NULL) return NULL;
    
    memset(icon, 0x00, sizeof(struct IconObject));
    icon->x = x;
    icon->y = y;
    icon->width = width;
    icon->height = height;
    icon->icon_sprite = sprite;
    icon->flags = 0; 
    icon->bounds_x = -1;
    icon->bounds_y = -1;
    icon->bounds_w = width + 2;
    icon->bounds_h = height + 2;
    icon->icon_index = icon_index;
    
    if (!list_append(&workspace.icon_head, &workspace.icon_tail, icon)) {
        free(icon);
        return NULL;
    }
    
    dwm_draw_redraw(icon->x, icon->y, icon->width, icon->height);
    return icon;
}

void dwm_destroy_icon(struct IconObject* icon) {
    DWM_GUARD();
    if (icon == NULL) return;
    
    size_t length = strlen(icon->name);
    uint16_t string_width = length * 6;
    
    int16_t min_x = icon->x + ((icon->width - string_width) / 2);
    int16_t max_x = min_x + string_width;
    if (min_x > icon->x) min_x = icon->x;
    if (max_x < icon->x + icon->width) max_x = icon->x + icon->width;
    
    dwm_invalidate_region(min_x, icon->y, max_x - min_x, icon->height + 40);
    
    bool explicitly_found = false;
    for (struct list_node* node = workspace.icon_head; node != NULL; node = node->next) {
        if (node->data == icon) {
            explicitly_found = true;
            break;
        }
    }
    if (!explicitly_found) return;
    
    // Don't leave the rename box pointing at a freed icon
    dwm_desktop_rename_forget_icon(icon);
    if (context.focused_icon == icon)      context.focused_icon = NULL;
    if (context.last_focused_icon == icon) context.last_focused_icon = NULL;
    if (dragdrop.dragged_icon == icon)     dragdrop.dragged_icon = NULL;
    
    list_remove(&workspace.icon_head, &workspace.icon_tail, icon);
    free(icon);
}

EditFieldHandle dwm_window_add_edit_field(WindowHandle handle, uint16_t x, uint16_t y, uint16_t width) {
    DWM_GUARD();
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) 
        return 0;
    
    struct WindowEditField* edit_field = malloc(sizeof(struct WindowEditField));
    if (edit_field == NULL) 
        return 0;
    
    edit_field->x = x;
    edit_field->y = y;
    edit_field->width = width;
    edit_field->height = 18;
    edit_field->is_active = true;
    edit_field->is_centered = true;
    
    uint16_t text_field_size = 128;
    
    edit_field->text = malloc(text_field_size);
    if (edit_field->text == NULL) {
        free(edit_field);
        return 0;
    }
    memset(edit_field->text, '\0', text_field_size);
    edit_field->cursor_index = 0;
    
    uint32_t candidate_id = workspace.next_edit_field_id++;
    if (candidate_id == 0) candidate_id = workspace.next_edit_field_id++;
    
    bool id_check = true;
    while (id_check) {
        id_check = false;
        for (struct list_node* node = workspace.window_head; node != NULL; node = node->next) {
            struct WindowObject* window = (struct WindowObject*)node->data;
            if (window->id == candidate_id) {
                id_check = true;
                candidate_id = workspace.next_edit_field_id++;
                if (candidate_id == 0) candidate_id = workspace.next_edit_field_id++;
                break;
            }
        }
    }
    
    edit_field->id = candidate_id;
    
    list_append(&window->edit_head, &window->edit_tail, edit_field);
    
    return candidate_id;
}

struct WindowObject* dwm_allocate_window(WindowClass w_class, uint16_t w_style, WindowProcedure proc) {
    // Held to the end: the window is fully built (buttons included) before
    // any other thread or frame can see it
    DWM_GUARD();
    
    struct WindowObject* window_object = malloc(sizeof(struct WindowObject));
    if (window_object == NULL) 
        return NULL;
    
    memset(window_object, 0x00, sizeof(struct WindowObject));
    uint32_t frame_buffer_sz = w_class.width * w_class.height * sizeof(uint32_t);
    
    if (w_style & DWM_WSTYLE_CHILD) {
        window_object->frame_buffer = NULL;
    } else {
        window_object->frame_buffer = (uint32_t*)malloc(frame_buffer_sz);
        if (window_object->frame_buffer == NULL) {
            free(window_object);
            return NULL;
        }
        memset(window_object->frame_buffer, 0x11, frame_buffer_sz);
    }
    
    uint32_t candidate_id = workspace.next_window_id++;
    if (candidate_id == 0) candidate_id = workspace.next_window_id++;
    
    bool id_check = true;
    while (id_check) {
        id_check = false;
        for (struct list_node* node = workspace.window_head; node != NULL; node = node->next) {
            struct WindowObject* window = (struct WindowObject*)node->data;
            if (window->id == candidate_id) {
                id_check = true;
                candidate_id = workspace.next_window_id++;
                if (candidate_id == 0) candidate_id = workspace.next_window_id++;
                break;
            }
        }
    }
    
    window_object->id = candidate_id;
    window_object->style = w_style;
    strncpy(window_object->title, w_class.title, DWM_MAX_TITLE_LEN);
    
    if (window_object->style & DWM_WSTYLE_NOBORDERS) {
        window_object->border_width = 0;
        window_object->titlebar_height = 0;
    } else if (window_object->style & DWM_WSTYLE_NOTITLEBAR) {
        window_object->border_width = 1;
        window_object->titlebar_height = 0;
    } else {
        window_object->border_width = 1;
        window_object->titlebar_height = 20;
    }
    
    window_object->parent = NULL;
    window_object->x = w_class.x;
    window_object->y = (w_class.y < 0) ? 0 : w_class.y;
    window_object->w = w_class.width;
    window_object->h = w_class.height;
    
    if (w_style & DWM_WSTYLE_CASCADE) {
        window_object->x = cascade.x;
        window_object->y = cascade.y;
        
        cascade.x += cascade.w;
        cascade.y += cascade.h;
        
        if (cascade.x > cascade.max) {
            cascade.x = cascade.w;
            cascade.y = cascade.h;
        }
    }
    
    window_object->local_x = 0;
    window_object->local_y = 0;
    int border_offset = 0;
    
    window_object->surface_w = window_object->w;
    window_object->surface_h = window_object->h - window_object->titlebar_height;
    window_object->buffer_w  = w_class.width;
    window_object->buffer_h  = w_class.height - window_object->titlebar_height - border_offset;
    
    window_object->surface_x = window_object->x;
    // The +1 skips the titlebar divider; titlebar-less windows have none
    window_object->surface_y = window_object->y + window_object->titlebar_height + (window_object->titlebar_height ? 1 : 0);
    window_object->buffer_w = w_class.width;
    window_object->buffer_h = w_class.height;
    
    window_object->max_width = w_class.max_width;
    window_object->max_height = w_class.max_height;

    window_object->border_color         = theme.frame.border;
    window_object->background_color     = theme.frame.background;
    window_object->title_text_color     = theme.frame.title_text;
    window_object->title_color_low      = theme.frame.active_low;
    window_object->title_color_high     = theme.frame.active_high;
    window_object->inactive_color_low   = theme.frame.inactive_low;
    window_object->inactive_color_high  = theme.frame.inactive_high;
    
    window_object->flags = DWM_WFLAG_REDRAW | DWM_WFLAG_REFRESH | DWM_WFLAG_REDECORATE;
    window_object->event_callback = proc;
    
    if (workspace.window_tail != NULL) {
        struct WindowObject* old_active = (struct WindowObject*)workspace.window_tail->data;
        old_active->flags |= DWM_WFLAG_REDECORATE;
        
        int old_abs_x, old_abs_y;
        dwm_get_absolute_position(old_active, &old_abs_x, &old_abs_y);
        dwm_invalidate_region(old_abs_x - old_active->border_width, 
                              old_abs_y - old_active->border_width, 
                              old_active->w + (old_active->border_width * 2), 
                              old_active->h + (old_active->border_width * 2));
    }
    
    if (!list_append(&workspace.window_head, &workspace.window_tail, window_object)) {
        free(window_object->frame_buffer);
        free(window_object);
        return NULL;
    }
    
    // A new window is appended at the tail, so it takes focus from the desktop
    workspace.desktop_focused = false;
    
    int redraw_x, redraw_y;
    dwm_get_absolute_position(window_object, &redraw_x, &redraw_y);
    dwm_draw_redraw(redraw_x - window_object->border_width, 
                    redraw_y - window_object->border_width, 
                    window_object->w + (window_object->border_width * 2), 
                    window_object->h + (window_object->border_width * 2));

    // Titlebar buttons need a titlebar to live in
    if (!(w_style & (DWM_WSTYLE_NOCLOSEBOX | DWM_WSTYLE_NOTITLEBAR))) {
        struct Image* button_close = dwm_resource_find("ui_close_red");
        struct Image* button_minimize  = dwm_resource_find("ui_minimize");
        
        if (button_close != NULL) {
            int16_t close_x = w_class.width - button_close->width;
            int16_t vertical = (window_object->titlebar_height / button_close->height) - 1;
            window_add_button(window_object, close_x, vertical, button_close->width, button_close->height, DWM_EVENT_CLOSE, button_close);
            
            if (button_minimize != NULL) {
                int16_t close_min = w_class.width - button_close->width - button_minimize->width;
                window_add_button(window_object, close_min, vertical, button_close->width, button_close->height, DWM_EVENT_MINIMIZE, button_minimize);
            }
        }
    }
    
    if (w_style & DWM_WSTYLE_RESIZEABLE) {
        uint16_t resize_w = 12;
        uint16_t resize_h = 12;
        int16_t resize_x = window_object->w - resize_w;
        int16_t resize_y = window_object->h - resize_h;
        window_add_button(window_object, resize_x, resize_y, resize_w, resize_h, DWM_EVENT_RESIZE, NULL);
    }
    
    dwm_post_message(window_object->id, DWM_EVENT_REDRAW, 0, 0);
    return window_object;
}

int8_t dwm_create_folder(uint16_t x, uint16_t y, const char* name, const char* path) {
    DWM_GUARD();
    struct Image* folder_sprite = dwm_resource_find("icon_folder");
    if (folder_sprite == NULL) 
        return -1;
    
    struct IconObject* folder = dwm_create_icon(x, y, folder_sprite->width, folder_sprite->height, folder_sprite, 0);
    if (folder == NULL)
        return -1;
    size_t name_length = strnlen(name, DWM_MAX_NAME_LEN);
    size_t path_length = strnlen(path, DWM_MAX_PATH_LEN);
    
    strncpy(folder->name, name, name_length);
    strncpy(folder->path, path, path_length);
    folder->name[name_length] = '\0';
    folder->path[path_length] = '\0';
    
    dwm_calculate_icon_bounds(folder);
    
    dwm_invalidate_region(x + folder->bounds_x, y + folder->bounds_y, folder->bounds_w, folder->bounds_h);
    return 0;
}

int8_t dwm_create_mount(uint16_t x, uint16_t y, const char* name, const char* path) {
    DWM_GUARD();
    struct Image* mount_sprite = dwm_resource_find("icon_storage");
    if (mount_sprite == NULL) 
        return -1;
    
    struct IconObject* folder = dwm_create_icon(x, y, mount_sprite->width, mount_sprite->height, mount_sprite, 0);
    if (folder == NULL)
        return -1;
    size_t name_length = strnlen(name, DWM_MAX_NAME_LEN);
    size_t path_length = strnlen(path, DWM_MAX_PATH_LEN);
    
    strncpy(folder->name, name, name_length);
    strncpy(folder->path, path, path_length);
    folder->name[name_length] = '\0';
    folder->path[path_length] = '\0';
    
    dwm_calculate_icon_bounds(folder);
    
    dwm_invalidate_region(x + folder->bounds_x, y + folder->bounds_y, folder->bounds_w, folder->bounds_h);
    return 0;
}

int8_t dwm_create_file(uint16_t x, uint16_t y, const char* name, const char* path) {
    DWM_GUARD();
    struct Image* file_sprite = dwm_resource_find("icon_file");
    if (file_sprite == NULL) 
        return -1;
    
    struct IconObject* file = dwm_create_icon(x, y, file_sprite->width, file_sprite->height, file_sprite, 1);
    if (file == NULL)
        return -1;
    size_t name_length = strnlen(name, DWM_MAX_NAME_LEN);
    size_t path_length = strnlen(path, DWM_MAX_PATH_LEN);
    
    strncpy(file->name, name, name_length);
    strncpy(file->path, path, path_length);
    file->name[name_length] = '\0';
    file->path[path_length] = '\0';
    
    dwm_calculate_icon_bounds(file);
    
    dwm_invalidate_region(x + file->bounds_x, y + file->bounds_y, file->bounds_w, file->bounds_h);
    return 0;
}

void dwm_summon_context_menu(WindowHandle window, uint16_t x, uint16_t y, const char** options, uint16_t number_of_items) {
    DWM_GUARD();
    struct WindowObject* w_object = dwm_get_window_by_id((uint32_t)window);
    if (w_object == NULL) 
        return;
    
    uint16_t posx = w_object->x + x;
    uint16_t posy = w_object->y + y + w_object->titlebar_height;
    
    // Check window bounds
    if ((posx > w_object->x + w_object->w) || (posy > w_object->y + w_object->h)) 
        return;
    
    // Set the window who called this context menu
    ctxmenu.handle = w_object;
    
    dwm_create_context_menu(posx, posy, DWM_CONTEXT_MENU_USER, options, number_of_items);
}

