#include <stdio.h>
#include <stdbool.h>

#include <kernel/kernel.h>
#include <kernel/dwm/dwm.h>
#include <kernel/events.h>
#include <kernel/malloc.h>
#include <kernel/arch/x86/irq.h>

#include <kernel/programs/explorer/internal.h>
#include <kernel/programs/explorer/explorer.h>

#include <kernel/util/string.h>
#include <kernel/util/tok.h>
#include <kernel/vfs/vfs.h>

struct Image* icon_folder = NULL;
struct Image* icon_file = NULL;
struct Image* icon_document = NULL;
struct Image* icon_system = NULL;
struct Image* icon_storage = NULL;

struct Image* ui_button_back;
struct Image* ui_button_new;

struct ExplorerWindowState* window_list_head = NULL;

void explorer_main(const char* arguments) {
    if (!icon_folder)     icon_folder      = dwm_resource_find("icon_folder");
    if (!icon_file)       icon_file        = dwm_resource_find("icon_file");
    if (!icon_system)     icon_system      = dwm_resource_find("icon_system");
    if (!icon_document)   icon_document    = dwm_resource_find("icon_document");
    if (!icon_storage)    icon_storage     = dwm_resource_find("icon_storage");
    
    if (!ui_button_back)  ui_button_back  = dwm_resource_find("ui_back");
    if (!ui_button_new)   ui_button_new   = dwm_resource_find("ui_new");
    
    char initial_path[MAX_PATH_LEN] = "/";
    
    if (arguments != NULL && arguments[0] != '\0') {
        strncpy(initial_path, arguments, MAX_PATH_LEN - 1);
        initial_path[MAX_PATH_LEN - 1] = '\0';
    }
    
    // Ensure the target path exists and is a directory
    if (!vfs_exists(initial_path) || !vfs_directory_check(initial_path)) {
        strncpy(initial_path, "/", MAX_PATH_LEN - 1);
    }
    
    char window_title[MAX_TITLE_LEN] = "/";
    if (strcmp(initial_path, "/") != 0) {
        const char* last_slash = strrchr(initial_path, '/');
        if (last_slash && *(last_slash + 1) != '\0') {
            strncpy(window_title, last_slash + 1, MAX_TITLE_LEN - 1);
        } else {
            strncpy(window_title, initial_path, MAX_TITLE_LEN - 1);
        }
    }
    window_title[MAX_TITLE_LEN - 1] = '\0';
    
    explorer_create_instance(window_title, initial_path);
}

struct ExplorerWindowState* allocate_window_state(WindowHandle handle) {
    struct ExplorerWindowState* new_node = (struct ExplorerWindowState*)malloc(sizeof(struct ExplorerWindowState));
    if (!new_node) return NULL;
    
    memset(new_node, 0, sizeof(struct ExplorerWindowState));
    new_node->handle = handle;
    new_node->win_width = WINDOW_WIDTH; 
    new_node->context_item_index = -1;
    
    // The list is read and modified from two threads: this runs on the
    // kernel thread (program launch), while free_window_state and
    // get_window_state run on the DWM thread. Link it in with interrupts
    // off so a preemption can't land between reading and writing the head.
    uint32_t flags = irq_save();
    new_node->next = window_list_head;
    window_list_head = new_node;
    irq_restore(flags);
    
    return new_node;
}

void free_window_state(WindowHandle handle) {
    struct ExplorerWindowState* found = NULL;
    
    // Unlink with interrupts off (see allocate_window_state), free outside
    uint32_t flags = irq_save();
    struct ExplorerWindowState* current = window_list_head;
    struct ExplorerWindowState* previous = NULL;
    
    while (current != NULL) {
        if (current->handle == handle) {
            if (previous == NULL) {
                window_list_head = current->next;
            } else {
                previous->next = current->next;
            }
            found = current;
            break;
        }
        previous = current;
        current = current->next;
    }
    irq_restore(flags);
    
    if (found != NULL) free(found);
}

WindowHandle explorer_create_instance(const char* title, const char* path) {
    struct ExplorerWindowState* state = allocate_window_state(0);
    if (state == NULL) return 0;
    
    populate_state_from_vfs(state, path);
    
    WindowClass wclass;
    memset(&wclass, 0x00, sizeof(WindowClass));
    wclass.x = 200;
    wclass.y = 150;
    wclass.width  = WINDOW_WIDTH;
    wclass.height = WINDOW_HEIGHT;
    wclass.max_width  = 0;
    wclass.max_height = 0;
    
    size_t title_length = strnlen(title, DWM_MAX_NAME_LEN);
    
    strncpy(wclass.title, title, title_length);
    wclass.title[title_length] = '\0';
    
    WindowHandle window = dwm_create_window(wclass, DWM_WSTYLE_RESIZEABLE, callback_handler_explorer);
    if (window == 0) {
        free_window_state(0);
        return 0;
    }
    state->handle = window;
    
    EditFieldHandle edit_field = dwm_window_add_edit_field(window, 0, 0, state->edit_width);
    dwm_window_edit_visible(window, edit_field, false);
    
    state->edit_handle = edit_field;
    
    // dwm_create_window releases the DWM lock before state->handle is set.
    // If the DWM thread ran a frame in that gap, the window's first REDRAW
    // reached callback_handler_explorer while get_window_state() could not
    // find this state yet, so nothing was drawn and the window stayed blank
    // until a refresh. Now that the state is reachable, ask for a paint.
    dwm_window_send_event(window, DWM_EVENT_REDRAW);
    
    dwm_window_set_focus(window);
    return window;
}

void populate_state_from_vfs(struct ExplorerWindowState* state, const char* target_path) {
    if (!state) return;

    char norm_path[MAX_PATH_LEN];
    if (!target_path || target_path[0] == '\0') {
        strncpy(norm_path, "/", MAX_PATH_LEN - 1);
    } else {
        strncpy(norm_path, target_path, MAX_PATH_LEN - 1);
    }
    norm_path[MAX_PATH_LEN - 1] = '\0';

    // Remove trailing slashes (except root)
    size_t len = strlen(norm_path);
    while (len > 1 && norm_path[len - 1] == '/') {
        norm_path[--len] = '\0';
    }

    strncpy(state->full_path, norm_path, MAX_PATH_LEN - 1);
    state->full_path[MAX_PATH_LEN - 1] = '\0';

    strncpy(state->path, norm_path, MAX_PATH_LEN - 1);
    state->path[MAX_PATH_LEN - 1] = '\0';

    if (strcmp(norm_path, "/") == 0) {
        strncpy(state->window_title, "/", MAX_TITLE_LEN - 1);
    } else {
        const char* last_slash = strrchr(norm_path, '/');
        if (last_slash) {
            strncpy(state->window_title, last_slash + 1, MAX_TITLE_LEN - 1);
        } else {
            strncpy(state->window_title, norm_path, MAX_TITLE_LEN - 1);
        }
    }
    state->window_title[MAX_TITLE_LEN - 1] = '\0';

    if (state->handle != 0) {
        dwm_window_set_name(state->handle, state->window_title);
    }

    // Determine VFS mount boundary for path color rendering
    state->knode_path_len = 0;
    state->fs_current = 0;

    char path_copy[MAX_PATH_LEN];
    strncpy(path_copy, norm_path, MAX_PATH_LEN - 1);
    path_copy[MAX_PATH_LEN - 1] = '\0';

    cstr_tok_t tok;
    cstr_tok_init(&tok, path_copy, "/");
    char curr_segment[MAX_PATH_LEN] = "";
    char* token = cstr_tok_next(&tok);

    bool found_mount = false;
    while (token != NULL) {
        strncat(curr_segment, "/", MAX_PATH_LEN - strlen(curr_segment) - 1);
        strncat(curr_segment, token, MAX_PATH_LEN - strlen(curr_segment) - 1);

        if (vfs_directory_check_mounted(curr_segment)) {
            // Set split point right before the mount directory segment
            const char* last_slash = strrchr(curr_segment, '/');
            if (last_slash != NULL) {
                state->knode_path_len = (uint16_t)(last_slash - curr_segment);
            } else {
                state->knode_path_len = 0;
            }

            found_mount = true;
            state->fs_current = 1;
            break;
        }
        token = cstr_tok_next(&tok);
    }

    if (!found_mount) {
        state->knode_path_len = (uint16_t)strlen(norm_path);
        state->fs_current = 0;
    }

    // Populate directory contents using VFS APIs
    memset(state->items, 0, sizeof(state->items));
    uint32_t count = vfs_directory_get_item_count(norm_path);
    uint16_t collected = 0;

    for (uint32_t i = 0; i < count && collected < MAX_ITEMS; i++) {
        char item_name[MAX_TITLE_LEN] = {0};

        if (!vfs_directory_get_item(norm_path, i, item_name, sizeof(item_name))) continue;
        if (item_name[0] == '\0') continue;
        if (strcmp(item_name, ".") == 0 || strcmp(item_name, "..") == 0) continue;

        struct Item* item = &state->items[collected];
        strncpy(item->name, item_name, MAX_TITLE_LEN - 1);
        item->name[MAX_TITLE_LEN - 1] = '\0';

        // Construct absolute VFS item path
        memset(item->path, '\0', MAX_PATH_LEN);
        strncpy(item->path, norm_path, MAX_PATH_LEN - 1);

        size_t path_len = strlen(item->path);
        if (path_len > 0 && item->path[path_len - 1] != '/' && path_len < MAX_PATH_LEN - 1) {
            item->path[path_len] = '/';
            path_len++;
        }

        if (path_len < MAX_PATH_LEN - 1) {
            strncat(item->path, item_name, MAX_PATH_LEN - path_len - 1);
        }

        if (vfs_directory_check_mounted(item->path)) {
            item->icon_index = ICON_STORAGE;
        } else if (vfs_directory_check(item->path)) {
            item->icon_index = ICON_FOLDER;
        } else {
            item->icon_index = ICON_FILE;
        }

        collected++;
    }

    state->total_items = collected;
}
